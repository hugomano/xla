/* Copyright 2024 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/backends/gpu/runtime/nccl_ragged_all_to_all_thunk.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/node_hash_map.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "absl/strings/substitute.h"
#include "absl/synchronization/mutex.h"
#include "absl/types/span.h"
#include "xla/backends/gpu/collectives/gpu_clique_key.h"
#include "xla/backends/gpu/collectives/gpu_collectives.h"
#include "xla/backends/gpu/runtime/nccl_collective_thunk.h"
#include "xla/backends/gpu/runtime/thunk.h"
#include "xla/core/collectives/communicator.h"
#include "xla/core/collectives/rank_id.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/service/collective_ops_utils.h"
#include "xla/service/gpu/transforms/collectives/collective_ops_utils.h"
#include "xla/service/rendezvous.h"
#include "xla/shape.h"
#include "xla/shape_util.h"
#include "xla/stream_executor/device_memory.h"
#include "xla/stream_executor/device_memory_handle.h"
#include "xla/stream_executor/event.h"
#include "xla/stream_executor/memory_allocation.h"
#include "xla/stream_executor/stream.h"
#include "xla/tsl/platform/errors.h"
#include "xla/tsl/platform/statusor.h"
#include "xla/xla_data.pb.h"

namespace xla {
namespace gpu {
namespace {

// RaggedAllToAll has 4 operands with ragged tensor metadata: input_offsets,
// send_sizes, output_offsets, and recv_sizes.
constexpr int64_t kNumRaggedMetadataOperands = 4;

NcclRaggedAllToAllConfig GetNcclRaggedAllToAllConfig(
    const HloRaggedAllToAllInstruction* instr) {
  NcclRaggedAllToAllConfig config;
  config.config = GetNcclCollectiveConfig(instr, std::nullopt);

  const Shape& input_size_shape = instr->operand(2)->shape();
  config.num_total_updates = input_size_shape.dimensions(0);
  config.ragged_row_element_size =
      ShapeUtil::ElementsIn(instr->shape()) / instr->shape().dimensions(0);
  return config;
}

// Loads the offsets and sizes of the input and output ragged tensors from
// device memory.
//
// The parameter `ragged_metadata_allocs` is a vector of pointers to the buffers
// in the host memory allocated by StreamExecutor to copy data from the device
// memory.
absl::Status LoadRaggedTensorMetadata(
    se::Stream& stream, const std::vector<DeviceBufferPair>& buffers,
    const std::vector<int64_t*>& ragged_metadata_allocs) {
  for (int64_t i = 0; i < kNumRaggedMetadataOperands; ++i) {
    TF_RETURN_IF_ERROR(stream.Memcpy(ragged_metadata_allocs[i],
                                     buffers[i + 2].source_buffer,
                                     buffers[i + 2].source_buffer.size()));
  }

  // Wait for the copies to complete.
  if (absl::Status blocked = stream.BlockHostUntilDone(); !blocked.ok()) {
    return absl::InternalError(absl::StrFormat(
        "Failed to complete all kernels launched on stream %p: %s", &stream,
        blocked.message()));
  }

  return absl::OkStatus();
}

// Runs AllToAll on a buffer that contains ragged tensor metadata.
absl::Status RunAllToAllOnIndexBuffer(
    GpuCollectives* collectives, const se::DeviceMemoryBase& source_buffer,
    int64_t num_updates_per_replica,
    const se::DeviceMemoryBase& destination_buffer, PrimitiveType element_type,
    se::Stream& stream, Communicator* comm) {
  TF_ASSIGN_OR_RETURN(int32_t num_ranks, comm->NumRanks());

  TF_RETURN_IF_ERROR(collectives->GroupStart());
  for (int peer = 0; peer < num_ranks; ++peer) {
    int64_t offset = peer * num_updates_per_replica;
    se::DeviceMemoryBase send_slice = collectives->Slice(
        source_buffer, element_type, offset, /*count=*/num_updates_per_replica);
    se::DeviceMemoryBase recv_slice =
        collectives->Slice(destination_buffer, element_type, offset,
                           /*count=*/num_updates_per_replica);

    TF_RETURN_IF_ERROR(comm->Send(send_slice, element_type,
                                  /*count=*/num_updates_per_replica,
                                  RankId(peer), GpuCollectives::On(stream)));

    TF_RETURN_IF_ERROR(comm->Recv(recv_slice, element_type,
                                  /*count=*/num_updates_per_replica,
                                  RankId(peer), GpuCollectives::On(stream)));
  }

  TF_RETURN_IF_ERROR(collectives->GroupEnd());
  return stream.BlockHostUntilDone();
}

absl::Status RunRaggedAllToAll(
    GpuCollectives* collectives, int64_t ragged_row_element_size,
    int64_t num_total_updates,
    const std::vector<DeviceBufferPair>& original_buffers, se::Stream& stream,
    Communicator* comm, const std::vector<int64_t*>& ragged_metadata_allocs,
    const se::DeviceMemoryBase& output_offsets_device_buffer) {
  int device_ordinal = stream.parent()->device_ordinal();
  VLOG(3) << "Performing ragged-all-to-all from device ordinal: "
          << device_ordinal;
  TF_RETURN_IF_ERROR(MaybeRegisterBuffers(collectives, stream.parent(),
                                          original_buffers, comm));

  TF_ASSIGN_OR_RETURN(int32_t num_ranks, comm->NumRanks());

  std::vector<DeviceBufferPair> buffers = original_buffers;

  int64_t num_updates_per_replica = num_total_updates / num_ranks;

  // `output_offsets` of the RaggedAllToAll instruction are sharded in a way,
  // that `output_offset[i]` is an offset in the i-th peer output buffer. To
  // make it work for NCCL model with send/recv, we need to know offsets in the
  // local output buffer. To get the correct offsets we perform an AllToAll on
  // the output_offsets buffer.
  DeviceBufferPair& output_offsets_buffer_pair = buffers[4];
  TF_RETURN_IF_ERROR(RunAllToAllOnIndexBuffer(
      collectives, output_offsets_buffer_pair.source_buffer,
      num_updates_per_replica, output_offsets_device_buffer,
      output_offsets_buffer_pair.element_type, stream, comm));
  output_offsets_buffer_pair.source_buffer = output_offsets_device_buffer;

  TF_RETURN_IF_ERROR(
      LoadRaggedTensorMetadata(stream, buffers, ragged_metadata_allocs));

  const int64_t* input_offsets = ragged_metadata_allocs[0];
  const int64_t* send_sizes = ragged_metadata_allocs[1];
  const int64_t* output_offsets = ragged_metadata_allocs[2];
  const int64_t* recv_sizes = ragged_metadata_allocs[3];

  TF_RETURN_IF_ERROR(collectives->GroupStart());

  PrimitiveType element_type = buffers[0].element_type;

  se::DeviceMemoryBase input_buffer = buffers[0].source_buffer;
  se::DeviceMemoryBase output_buffer = buffers[1].destination_buffer;

  for (int64_t i = 0; i < num_updates_per_replica; ++i) {
    for (int peer = 0; peer < num_ranks; ++peer) {
      int64_t idx = peer * num_updates_per_replica + i;
      se::DeviceMemoryBase send_slice =
          collectives->Slice(input_buffer, element_type,
                             input_offsets[idx] * ragged_row_element_size,
                             send_sizes[idx] * ragged_row_element_size);

      se::DeviceMemoryBase recv_slice =
          collectives->Slice(output_buffer, element_type,
                             output_offsets[idx] * ragged_row_element_size,
                             recv_sizes[idx] * ragged_row_element_size);

      TF_RETURN_IF_ERROR(comm->Send(send_slice, element_type,
                                    send_sizes[idx] * ragged_row_element_size,
                                    RankId(peer), GpuCollectives::On(stream)));

      TF_RETURN_IF_ERROR(comm->Recv(recv_slice, element_type,
                                    recv_sizes[idx] * ragged_row_element_size,
                                    RankId(peer), GpuCollectives::On(stream)));
    }
  }

  return collectives->GroupEnd();
}

// Contains the values that are passed between host threads with rendezvous.
struct RendezvousValue {
  RankId rank;
  se::DeviceMemoryBase output_buffer;
  se::Event* start_event;
  se::Event* end_event;

  bool operator<(const RendezvousValue& other) const {
    return rank < other.rank;
  }
};

// TODO(b/380457503): Memcpy AllToAll implementation must be moved to
// NcclCommunicator implementation.
absl::Status RunMemCpyRaggedAllToAll(
    GpuCollectives* collectives, const GpuCliqueKey& clique_key, RankId rank,
    int64_t ragged_row_element_size, int64_t num_total_updates,
    const std::vector<DeviceBufferPair>& buffers, se::Stream& stream,
    Communicator* comm, const std::vector<int64_t*>& ragged_metadata_allocs,
    se::Event* start_event, se::Event* end_event) {
  int device_ordinal = stream.parent()->device_ordinal();
  VLOG(3) << "Performing mem-copy-ragged-all-to-all from device ordinal: "
          << device_ordinal;
  TF_RETURN_IF_ERROR(
      MaybeRegisterBuffers(collectives, stream.parent(), buffers, comm));

  TF_ASSIGN_OR_RETURN(int32_t num_ranks, comm->NumRanks());

  PrimitiveType element_type = buffers[0].element_type;

  se::DeviceMemoryBase input_buffer = buffers[0].source_buffer;
  se::DeviceMemoryBase output_buffer = buffers[1].destination_buffer;

  TF_RETURN_IF_ERROR(
      LoadRaggedTensorMetadata(stream, buffers, ragged_metadata_allocs));

  int64_t num_updates_per_replica = num_total_updates / num_ranks;

  const int64_t* input_offsets = ragged_metadata_allocs[0];
  const int64_t* send_sizes = ragged_metadata_allocs[1];
  const int64_t* output_offsets = ragged_metadata_allocs[2];

  RendezvousValue rendezvous_value;
  rendezvous_value.rank = rank;
  rendezvous_value.output_buffer = output_buffer;
  rendezvous_value.start_event = start_event;
  rendezvous_value.end_event = end_event;

  // Record that this device has started the memcpy ragged-all-to-all. We do
  // this before the rendezvous to make sure that RecordEvent is called before
  // WaitFor on another stream.
  TF_RETURN_IF_ERROR(stream.RecordEvent(start_event));

  auto rendezvous_fn = [](absl::Span<const RendezvousValue* const> values) {
    std::vector<RendezvousValue> values_copy;
    for (const auto& value : values) {
      values_copy.push_back(*value);
    }
    // Sort to make sure that values are in the same order as the devices are
    // ordered in the communicator.
    absl::c_sort(values_copy);
    return values_copy;
  };

  std::string start_rendezvous_key =
      absl::StrFormat("start memcpy ragged-all-to-all for rank %d, clique %s",
                      rank.value(), clique_key.ToString());
  std::shared_ptr<std::vector<RendezvousValue>> rendezvous_values =
      Rendezvous<std::vector<RendezvousValue>>(
          /*name=*/
          start_rendezvous_key, /*key=*/clique_key,
          /*value=*/rendezvous_value, /*num_threads=*/num_ranks, rendezvous_fn);

  // Wait for all devices to reach the start event. This indicates that all
  // output buffers are ready for transfer.
  for (auto& value : *rendezvous_values) {
    TF_RETURN_IF_ERROR(stream.WaitFor(value.start_event));
  }

  // Transfer a slice of data to each peer's output buffer.
  for (int64_t i = 0; i < num_updates_per_replica; ++i) {
    for (int peer = 0; peer < num_ranks; ++peer) {
      int64_t idx = peer * num_updates_per_replica + i;
      se::DeviceMemoryBase send_slice =
          collectives->Slice(input_buffer, element_type,
                             input_offsets[idx] * ragged_row_element_size,
                             send_sizes[idx] * ragged_row_element_size);
      se::DeviceMemoryBase dst_slice = collectives->Slice(
          (*rendezvous_values)[peer].output_buffer, element_type,
          output_offsets[idx] * ragged_row_element_size,
          send_sizes[idx] * ragged_row_element_size);
      TF_RETURN_IF_ERROR(
          stream.MemcpyD2D(&dst_slice, send_slice, send_slice.size()));
    }
  }

  // Record that this device has finished the memcpy ragged-all-to-all.
  TF_RETURN_IF_ERROR(stream.RecordEvent(end_event));

  // Do another rendezvous to make sure that we call RecordEvent for end_event
  // before WaitFor on another stream.
  std::string finish_rendezvous_key =
      absl::StrFormat("finish memcpy ragged-all-to-all for rank %d, clique %s",
                      rank.value(), clique_key.ToString());
  Rendezvous(/*name=*/finish_rendezvous_key,
             /*key=*/clique_key,
             /*num_threads=*/num_ranks);

  // Wait for all devices to reach the end event. This indicates that all
  // updates from other devices have arrived.
  for (auto& value : *rendezvous_values) {
    TF_RETURN_IF_ERROR(stream.WaitFor(value.end_event));
  }

  return absl::OkStatus();
}

}  // namespace

NcclRaggedAllToAllStartThunk::NcclRaggedAllToAllStartThunk(
    ThunkInfo thunk_info, const HloRaggedAllToAllInstruction* instr,
    std::vector<NcclCollectiveThunk::Buffer> buffers, bool p2p_memcpy_enabled)
    : NcclCollectiveThunk(Thunk::kNcclRaggedAllToAllStart, thunk_info,
                          IsGPUSyncCollective(*instr),
                          AsyncStreamKind::kCollective),
      config_(GetNcclRaggedAllToAllConfig(instr)),
      buffers_(std::move(buffers)),
      p2p_memcpy_enabled_(p2p_memcpy_enabled) {
  CHECK_EQ(config_.config.operand_count, buffers_.size());
}

/*static*/ absl::Status NcclRaggedAllToAllStartThunk::CheckImplementable(
    const HloRaggedAllToAllInstruction* instr, int64_t replica_count,
    int64_t partition_count) {
  auto status = [&instr]() -> absl::Status {
    for (HloInstruction* operand : instr->operands()) {
      Shape shape = operand->shape();
      TF_RETURN_IF_ERROR(IsValidOperand(shape, Thunk::kNcclRaggedAllToAll));
    }

    if (!ShapeUtil::IsEffectivelyMostMajorDimension(instr->shape(), 0)) {
      return absl::UnimplementedError(absl::Substitute(
          "ragged-all-to-all must have the ragged dimension (0) in the most "
          "major position in the layout $0.",
          instr->shape().layout().ToString()));
    }

    if (instr->operand(2)->shape().element_type() != S64) {
      return absl::InvalidArgumentError(
          "RaggedAllToAllDecomposer only supports S64 offsets. Was "
          "`ragged-all-to-all-canonicalizer` pass executed?");
    }

    return absl::OkStatus();
  };
  return AddOpDescription<NcclRaggedAllToAllStartThunk>(
      status(), instr, replica_count, partition_count);
}

/*static*/ CollectiveOpGroupMode NcclRaggedAllToAllStartThunk::GetGroupMode(
    const HloRaggedAllToAllInstruction* instr) {
  return GetNcclRaggedAllToAllConfig(instr).config.group_mode;
}

absl::Status NcclRaggedAllToAllStartThunk::Initialize(
    const InitializeParams& params) {
  TF_RETURN_IF_ERROR(NcclCollectiveThunk::Initialize(params));
  device_count_ = params.local_device_count;

  // Allocate temp buffers in the host memory to load the sizes and offsets of
  // ragged tensors from device memory.
  absl::MutexLock lock(&mutex_);
  if (!host_buffer_allocs_.contains(params.executor)) {
    std::vector<std::unique_ptr<se::MemoryAllocation>> allocs;
    for (int64_t i = 0; i < kNumRaggedMetadataOperands; ++i) {
      TF_ASSIGN_OR_RETURN(std::unique_ptr<se::MemoryAllocation> alloc,
                          params.executor->HostMemoryAllocate(
                              config_.num_total_updates * sizeof(int64_t)));
      allocs.push_back(std::move(alloc));
    }
    host_buffer_allocs_.emplace(params.executor, std::move(allocs));
  }

  if (!device_buffer_allocs_.contains(params.executor)) {
    se::DeviceMemoryHandle output_offsets_device_buffer{
        params.executor,
        params.executor->Allocate(config_.num_total_updates * sizeof(int64_t))};

    if (output_offsets_device_buffer.memory().is_null()) {
      return absl::InternalError("Failed to allocate output offsets buffer.");
    }

    device_buffer_allocs_.emplace(params.executor,
                                  std::move(output_offsets_device_buffer));
  }

  if (should_use_memcpy()) {
    se::StreamExecutor* executor = params.executor;
    {
      absl::MutexLock lock(&events_mutex_);
      if (!start_events_.count(executor)) {
        TF_ASSIGN_OR_RETURN(std::unique_ptr<se::Event> event,
                            executor->CreateEvent());
        start_events_.insert({executor, std::move(event)});
      }

      if (!end_events_.count(executor)) {
        TF_ASSIGN_OR_RETURN(std::unique_ptr<se::Event> event,
                            executor->CreateEvent());
        end_events_.insert({executor, std::move(event)});
      }
    }
  }
  return absl::OkStatus();
}

bool NcclRaggedAllToAllStartThunk::is_local() const {
  CHECK_NE(device_count_, -1);
  for (const auto& replica_group : config_.config.replica_groups) {
    const int64_t node_id = replica_group.replica_ids().at(0) / device_count_;
    if (!absl::c_all_of(replica_group.replica_ids(),
                        [this, node_id](const int64_t rank) {
                          return rank / device_count_ == node_id;
                        })) {
      return false;
    }
  }
  return true;
}

absl::Status NcclRaggedAllToAllStartThunk::RunNcclCollective(
    const ExecuteParams& params, se::Stream& stream,
    CommunicatorHandle comm_handle) {
  TF_ASSIGN_OR_RETURN(
      std::vector<DeviceBufferPair> device_buffers,
      ConvertToDeviceBuffers(params, buffers_,
                             config_.config.operand_element_type));

  TF_ASSIGN_OR_RETURN(GpuCollectives * collectives, GetGpuCollectives(params));

  // Get buffer allocs to load sizes and offsets of ragged tensors from device
  // memory.
  std::vector<int64_t*> ragged_metadata_allocs(kNumRaggedMetadataOperands);
  se::DeviceMemoryBase output_offsets_device_buffer;
  {
    absl::MutexLock lock(&mutex_);
    auto it = host_buffer_allocs_.find(stream.parent());
    CHECK(it != host_buffer_allocs_.end());

    for (int64_t i = 0; i < kNumRaggedMetadataOperands; ++i) {
      ragged_metadata_allocs[i] =
          reinterpret_cast<int64_t*>(it->second[i]->opaque());
    }

    auto jt = device_buffer_allocs_.find(stream.parent());
    CHECK(jt != device_buffer_allocs_.end());
    output_offsets_device_buffer = jt->second.memory();
  }

  if (should_use_memcpy()) {
    se::Event* start_event = nullptr;
    se::Event* end_event = nullptr;
    {
      absl::MutexLock lock(&events_mutex_);
      start_event = start_events_[stream.parent()].get();
      end_event = end_events_[stream.parent()].get();
    }
    TF_ASSIGN_OR_RETURN(
        GpuCliqueKey clique_key,
        GetGpuCliqueKey(collectives, *params.collective_params,
                        config().replica_groups, config().group_mode,
                        nccl_stream_id(), GetAsyncStreamKind()));

    std::optional<RankId> rank =
        clique_key.rank(params.collective_params->global_device_id);

    return RunMemCpyRaggedAllToAll(
        collectives, clique_key, *rank, config_.ragged_row_element_size,
        config_.num_total_updates, device_buffers, stream, comm_handle.comm,
        ragged_metadata_allocs, start_event, end_event);
  }

  return RunRaggedAllToAll(collectives, config_.ragged_row_element_size,
                           config_.num_total_updates, device_buffers, stream,
                           comm_handle.comm, ragged_metadata_allocs,
                           output_offsets_device_buffer);
}

}  // namespace gpu
}  // namespace xla
