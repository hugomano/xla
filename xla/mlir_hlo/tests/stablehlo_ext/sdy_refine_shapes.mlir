// RUN: mlir-hlo-opt %s -stablehlo-ext-refine-shapes --split-input-file 2>&1 | FileCheck %s

// Only the operand is manual.

sdy.mesh @mesh = <["a"=2, "b"=2]>

// CHECK-LABEL: func @main
// CHECK-SAME:  (%arg0: tensor<16x32xf32>) -> tensor<8x32xf32>
func.func @main(%arg0: tensor<16x32xf32>) -> tensor<?x32xf32> {
  // CHECK-NEXT: %[[ADD:.*]] = stablehlo.add %arg0, %arg0 : tensor<16x32xf32>
  // CHECK-NEXT: %[[MC:.*]] = sdy.manual_computation(%0)
  // CHECK-SAME:     in_shardings=[<@mesh, [{"a", ?}, {?}]>]
  // CHECK-SAME:     out_shardings=[<@mesh, [{?}, {?}], replicated={"a"}>]
  // CHECK-SAME:     manual_axes={"a"} (%arg1: tensor<8x32xf32>) {
  // CHECK-NEXT:  %[[ADD_2:.*]] = stablehlo.add %arg1, %arg1 : tensor<8x32xf32>
  // CHECK-NEXT:  sdy.return %[[ADD_2]] : tensor<8x32xf32>
  // CHECK-NEXT: } : (tensor<16x32xf32>) -> tensor<8x32xf32>
  // CHECK-NEXT: return %[[MC]] : tensor<8x32xf32>
  %0 = stablehlo.add %arg0, %arg0 : (tensor<16x32xf32>, tensor<16x32xf32>) -> tensor<?x32xf32>
  %1 = sdy.manual_computation(%0) in_shardings=[<@mesh, [{"a", ?}, {?}]>] out_shardings=[<@mesh, [{?}, {?}], replicated={"a"}>] manual_axes={"a"} (%arg1: tensor<?x32xf32>) {
    %2 = stablehlo.add %arg1, %arg1 : tensor<?x32xf32>
    sdy.return %2 : tensor<?x32xf32>
  } : (tensor<?x32xf32>) -> tensor<?x32xf32>
  return %1: tensor<?x32xf32>
}

// -----

// Only the result is manual.

sdy.mesh @mesh = <["a"=2, "b"=2]>

// CHECK-LABEL: func @main
// CHECK-SAME:  (%arg0: tensor<16x32xf32>) -> tensor<32x32xf32>
func.func @main(%arg0: tensor<16x32xf32>) -> tensor<?x32xf32> {
  // CHECK-NEXT: %[[ADD:.*]] = stablehlo.add %arg0, %arg0 : tensor<16x32xf32>
  // CHECK-NEXT: %[[MC:.*]] = sdy.manual_computation(%0)
  // CHECK-SAME:     in_shardings=[<@mesh, [{?}, {?}], replicated={"a"}>]
  // CHECK-SAME:     out_shardings=[<@mesh, [{"a", ?}, {?}]>]
  // CHECK-SAME:     manual_axes={"a"} (%arg1: tensor<16x32xf32>) {
  // CHECK-NEXT:  %[[ADD_2:.*]] = stablehlo.add %arg1, %arg1 : tensor<16x32xf32>
  // CHECK-NEXT:  sdy.return %[[ADD_2]] : tensor<16x32xf32>
  // CHECK-NEXT: } : (tensor<16x32xf32>) -> tensor<32x32xf32>
  // CHECK-NEXT: return %[[MC]] : tensor<32x32xf32>
  %0 = stablehlo.add %arg0, %arg0 : (tensor<16x32xf32>, tensor<16x32xf32>) -> tensor<?x32xf32>
  %1 = sdy.manual_computation(%0) in_shardings=[<@mesh, [{?}, {?}], replicated={"a"}>] out_shardings=[<@mesh, [{"a", ?}, {?}]>] manual_axes={"a"} (%arg1: tensor<?x32xf32>) {
    %2 = stablehlo.add %arg1, %arg1 : tensor<?x32xf32>
    sdy.return %2 : tensor<?x32xf32>
  } : (tensor<?x32xf32>) -> tensor<?x32xf32>
  return %1: tensor<?x32xf32>
}

// -----

// Both operand and result are manual.

sdy.mesh @mesh = <["a"=2, "b"=2]>

// CHECK-LABEL: func @main
// CHECK-SAME:  (%arg0: tensor<16x32xf32>) -> tensor<16x32xf32>
func.func @main(%arg0: tensor<16x32xf32>) -> tensor<?x32xf32> {
  // CHECK-NEXT: %[[ADD:.*]] = stablehlo.add %arg0, %arg0 : tensor<16x32xf32>
  // CHECK-NEXT: %[[MC:.*]] = sdy.manual_computation(%0)
  // CHECK-SAME:     in_shardings=[<@mesh, [{"a", ?}, {?}]>]
  // CHECK-SAME:     out_shardings=[<@mesh, [{"a", ?}, {?}]>]
  // CHECK-SAME:     manual_axes={"a"} (%arg1: tensor<8x32xf32>) {
  // CHECK-NEXT:  %[[ADD_2:.*]] = stablehlo.add %arg1, %arg1 : tensor<8x32xf32>
  // CHECK-NEXT:  sdy.return %[[ADD_2]] : tensor<8x32xf32>
  // CHECK-NEXT: } : (tensor<16x32xf32>) -> tensor<16x32xf32>
  // CHECK-NEXT: return %[[MC]] : tensor<16x32xf32>
  %0 = stablehlo.add %arg0, %arg0 : (tensor<16x32xf32>, tensor<16x32xf32>) -> tensor<?x32xf32>
  %1 = sdy.manual_computation(%0) in_shardings=[<@mesh, [{"a", ?}, {?}]>] out_shardings=[<@mesh, [{"a", ?}, {?}]>] manual_axes={"a"} (%arg1: tensor<?x32xf32>) {
    %2 = stablehlo.add %arg1, %arg1 : tensor<?x32xf32>
    sdy.return %2 : tensor<?x32xf32>
  } : (tensor<?x32xf32>) -> tensor<?x32xf32>
  return %1: tensor<?x32xf32>
}

// -----

// The dimension being refined is not the one which is manually sharded.

sdy.mesh @mesh = <["x"=2]>

// CHECK-LABEL: func @main
// CHECK-SAME:  (%arg0: tensor<4x4xf32>) -> tensor<4x4xf32>
func.func @main(%arg0: tensor<4x4xf32>) -> tensor<4x?xf32> {
  // CHECK-NEXT: %[[ABS:.*]] = stablehlo.abs %arg0 : tensor<4x4xf32>
  // CHECK-NEXT: %[[MC:.*]] = sdy.manual_computation(%[[ABS]])
  // CHECK-SAME:     in_shardings=[<@mesh, [{"x"}, {}]>]
  // CHECK-SAME:     out_shardings=[<@mesh, [{"x"}, {}]>]
  // CHECK-SAME:     manual_axes={"x"} (%arg1: tensor<2x4xf32>) {
  // CHECK-NEXT:  %[[ADD:.*]] = stablehlo.add %arg1, %arg1 : tensor<2x4xf32>
  // CHECK-NEXT:  sdy.return %[[ADD]] : tensor<2x4xf32>
  // CHECK-NEXT: } : (tensor<4x4xf32>) -> tensor<4x4xf32>
  // CHECK-NEXT: return %[[MC]] : tensor<4x4xf32>
  %9 = stablehlo.abs %arg0 : (tensor<4x4xf32>) -> tensor<4x?xf32>
  %0 = sdy.manual_computation(%9) in_shardings=[<@mesh, [{"x"}, {}]>] out_shardings=[<@mesh, [{"x"}, {}]>] manual_axes={"x"} (%arg1: tensor<2x?xf32>) {
    %1 = stablehlo.add %arg1, %arg1 : tensor<2x?xf32>
    sdy.return %1 : tensor<2x?xf32>
  } : (tensor<4x?xf32>) -> tensor<4x?xf32>
  return %0 : tensor<4x?xf32>
}

// -----

// Body of named computation has all SDY operations.

sdy.mesh @mesh = <["a"=2, "b"=2]>

// CHECK-LABEL: func @main
// CHECK-SAME:  (%arg0: tensor<16x32xf32>) -> tensor<16x32xf32>
func.func @main(%arg0: tensor<16x32xf32>) -> tensor<?x32xf32> {
  // CHECK-NEXT: %[[ABS:.*]] = stablehlo.abs %arg0 : tensor<16x32xf32>
  // CHECK-NEXT: %[[NC:.*]] = sdy.named_computation<"foo">(%[[ABS]]) (%arg1: tensor<16x32xf32>) {
  // CHECK-NEXT:   %[[SC:.*]] = sdy.sharding_constraint %arg1 <@mesh, [{"b"}, {?}]> : tensor<16x32xf32>
  // CHECK-NEXT:   %[[RESHARD:.*]] = sdy.reshard %[[SC]] <@mesh, [{"b", "a"}, {?}]> : tensor<16x32xf32>
  // CHECK-NEXT:   sdy.sharding_group %[[RESHARD]] group_id=0 : tensor<16x32xf32>
  // CHECK-NEXT:   sdy.return %[[RESHARD]] : tensor<16x32xf32>
  // CHECK-NEXT: } : (tensor<16x32xf32>) -> tensor<16x32xf32>
  // CHECK-NEXT: return %[[NC]] : tensor<16x32xf32>
  %0 = stablehlo.abs %arg0 : (tensor<16x32xf32>) -> tensor<?x32xf32>
  %1 = sdy.named_computation<"foo">(%0) (%arg1: tensor<?x32xf32>) {
    %2 = sdy.sharding_constraint %arg1 <@mesh, [{"b"}, {?}]> : tensor<?x32xf32>
    %3 = sdy.reshard %2 <@mesh, [{"b", "a"}, {?}]> : tensor<?x32xf32>
    sdy.sharding_group %3 group_id=0 : tensor<?x32xf32>
    sdy.return %3 : tensor<?x32xf32>
  } : (tensor<?x32xf32>) -> tensor<?x32xf32>
  return %1: tensor<?x32xf32>
}
