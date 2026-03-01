// Generated with `xb buildshaders`.
#if 0
; SPIR-V
; Version: 1.0
; Generator: Khronos Glslang Reference Front End; 11
; Bound: 25190
; Schema: 0
               OpCapability Shader
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %5663 "main" %gl_WorkGroupID %gl_LocalInvocationID
               OpExecutionMode %5663 LocalSize 32 32 1
               OpDecorate %gl_WorkGroupID BuiltIn WorkgroupId
               OpDecorate %_struct_1056 Block
               OpMemberDecorate %_struct_1056 0 Offset 0
               OpMemberDecorate %_struct_1056 1 Offset 4
               OpMemberDecorate %_struct_1056 2 Offset 8
               OpMemberDecorate %_struct_1056 3 Offset 12
               OpMemberDecorate %_struct_1056 4 Offset 16
               OpMemberDecorate %_struct_1056 5 Offset 20
               OpDecorate %gl_LocalInvocationID BuiltIn LocalInvocationId
               OpDecorate %_runtimearr_uint ArrayStride 4
               OpDecorate %_struct_1948 BufferBlock
               OpMemberDecorate %_struct_1948 0 NonWritable
               OpMemberDecorate %_struct_1948 0 Offset 0
               OpDecorate %3152 NonWritable
               OpDecorate %3152 Binding 0
               OpDecorate %3152 DescriptorSet 0
               OpDecorate %_runtimearr_uint_0 ArrayStride 4
               OpDecorate %_struct_1949 BufferBlock
               OpMemberDecorate %_struct_1949 0 NonReadable
               OpMemberDecorate %_struct_1949 0 Offset 0
               OpDecorate %5522 NonReadable
               OpDecorate %5522 Binding 1
               OpDecorate %5522 DescriptorSet 0
               OpDecorate %gl_WorkGroupSize BuiltIn WorkgroupSize
       %void = OpTypeVoid
       %1282 = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %v3uint = OpTypeVector %uint 3
%_ptr_Input_v3uint = OpTypePointer Input %v3uint
%gl_WorkGroupID = OpVariable %_ptr_Input_v3uint Input
     %uint_0 = OpConstant %uint 0
%_ptr_Input_uint = OpTypePointer Input %uint
%_struct_1056 = OpTypeStruct %uint %uint %uint %uint %uint %uint
%_ptr_PushConstant__struct_1056 = OpTypePointer PushConstant %_struct_1056
       %4930 = OpVariable %_ptr_PushConstant__struct_1056 PushConstant
        %int = OpTypeInt 32 1
      %int_3 = OpConstant %int 3
%_ptr_PushConstant_uint = OpTypePointer PushConstant %uint
       %bool = OpTypeBool
%gl_LocalInvocationID = OpVariable %_ptr_Input_v3uint Input
     %uint_1 = OpConstant %uint 1
    %uint_32 = OpConstant %uint 32
      %int_2 = OpConstant %int 2
  %uint_1024 = OpConstant %uint 1024
      %int_0 = OpConstant %int 0
      %int_1 = OpConstant %int 1
      %int_5 = OpConstant %int 5
      %int_4 = OpConstant %int 4
     %uint_2 = OpConstant %uint 2
%_runtimearr_uint = OpTypeRuntimeArray %uint
%_struct_1948 = OpTypeStruct %_runtimearr_uint
%_ptr_Uniform__struct_1948 = OpTypePointer Uniform %_struct_1948
       %3152 = OpVariable %_ptr_Uniform__struct_1948 Uniform
%_ptr_Uniform_uint = OpTypePointer Uniform %uint
     %uint_3 = OpConstant %uint 3
     %uint_8 = OpConstant %uint 8
   %uint_255 = OpConstant %uint 255
%_arr_uint_uint_1024 = OpTypeArray %uint %uint_1024
%_ptr_Workgroup__arr_uint_uint_1024 = OpTypePointer Workgroup %_arr_uint_uint_1024
       %4017 = OpVariable %_ptr_Workgroup__arr_uint_uint_1024 Workgroup
%_ptr_Workgroup_uint = OpTypePointer Workgroup %uint
   %uint_264 = OpConstant %uint 264
%_runtimearr_uint_0 = OpTypeRuntimeArray %uint
%_struct_1949 = OpTypeStruct %_runtimearr_uint_0
%_ptr_Uniform__struct_1949 = OpTypePointer Uniform %_struct_1949
       %5522 = OpVariable %_ptr_Uniform__struct_1949 Uniform
   %uint_256 = OpConstant %uint 256
 %uint_65535 = OpConstant %uint 65535
    %uint_16 = OpConstant %uint 16
   %uint_512 = OpConstant %uint 512
%gl_WorkGroupSize = OpConstantComposite %v3uint %uint_32 %uint_32 %uint_1
       %5663 = OpFunction %void None %1282
      %15110 = OpLabel
               OpSelectionMerge %14903 None
               OpSwitch %uint_0 %11880
      %11880 = OpLabel
      %22245 = OpAccessChain %_ptr_Input_uint %gl_WorkGroupID %uint_0
      %15627 = OpLoad %uint %22245
      %22225 = OpAccessChain %_ptr_PushConstant_uint %4930 %int_3
       %7085 = OpLoad %uint %22225
       %7405 = OpUGreaterThanEqual %bool %15627 %7085
               OpSelectionMerge %16413 None
               OpBranchConditional %7405 %21992 %16413
      %21992 = OpLabel
               OpBranch %14903
      %16413 = OpLabel
      %20632 = OpAccessChain %_ptr_Input_uint %gl_LocalInvocationID %uint_1
      %15628 = OpLoad %uint %20632
      %21427 = OpAccessChain %_ptr_Input_uint %gl_LocalInvocationID %uint_0
      %12090 = OpLoad %uint %21427
       %6234 = OpIMul %uint %15628 %uint_32
       %8353 = OpIAdd %uint %6234 %12090
      %16525 = OpAccessChain %_ptr_PushConstant_uint %4930 %int_2
      %13159 = OpLoad %uint %16525
      %15178 = OpShiftLeftLogical %uint %uint_1 %13159
      %21815 = OpIMul %uint %uint_1024 %15178
      %13000 = OpAccessChain %_ptr_PushConstant_uint %4930 %int_0
      %18628 = OpLoad %uint %13000
      %21428 = OpAccessChain %_ptr_PushConstant_uint %4930 %int_1
      %12166 = OpLoad %uint %21428
      %24613 = OpIMul %uint %18628 %12166
      %13723 = OpIMul %uint %21815 %24613
       %8597 = OpAccessChain %_ptr_PushConstant_uint %4930 %int_5
      %21470 = OpLoad %uint %8597
      %24311 = OpINotEqual %bool %21470 %uint_0
      %23629 = OpUGreaterThan %bool %24613 %uint_1
      %11037 = OpLogicalAnd %bool %24311 %23629
               OpSelectionMerge %23266 None
               OpBranchConditional %11037 %11410 %23266
      %11410 = OpLabel
      %20167 = OpShiftRightLogical %uint %18628 %uint_1
      %24779 = OpShiftRightLogical %uint %12166 %uint_1
      %16010 = OpIMul %uint %24779 %18628
      %19240 = OpIAdd %uint %20167 %16010
               OpBranch %23266
      %23266 = OpLabel
      %19748 = OpPhi %uint %uint_0 %16413 %19240 %11410
      %25074 = OpAccessChain %_ptr_PushConstant_uint %4930 %int_4
      %25189 = OpLoad %uint %25074
       %7507 = OpIMul %uint %15627 %13723
      %17182 = OpIAdd %uint %25189 %7507
      %19692 = OpIMul %uint %8353 %15178
      %16238 = OpIMul %uint %19692 %24613
       %6869 = OpIAdd %uint %17182 %16238
      %21638 = OpIMul %uint %19748 %15178
      %18521 = OpIAdd %uint %6869 %21638
       %7572 = OpShiftRightLogical %uint %18521 %uint_2
       %6935 = OpIMul %uint %15627 %21815
       %9665 = OpIAdd %uint %6935 %19692
       %9085 = OpShiftRightLogical %uint %9665 %uint_2
               OpSelectionMerge %18023 None
               OpSwitch %13159 %18023 0 %11881 1 %16414 2 %16415 3 %8158
      %11881 = OpLabel
      %24791 = OpAccessChain %_ptr_Uniform_uint %3152 %int_0 %7572
      %12865 = OpLoad %uint %24791
      %12121 = OpBitwiseAnd %uint %18521 %uint_3
      %13987 = OpIMul %uint %12121 %uint_8
      %14461 = OpShiftRightLogical %uint %12865 %13987
      %15580 = OpBitwiseAnd %uint %14461 %uint_255
      %19824 = OpShiftRightLogical %uint %8353 %uint_2
      %19243 = OpBitwiseAnd %uint %8353 %uint_3
      %14114 = OpIMul %uint %19243 %uint_8
      %23170 = OpShiftLeftLogical %uint %15580 %14114
      %15629 = OpIEqual %bool %19243 %uint_0
               OpSelectionMerge %20883 None
               OpBranchConditional %15629 %12148 %20883
      %12148 = OpLabel
      %21533 = OpAccessChain %_ptr_Workgroup_uint %4017 %19824
               OpStore %21533 %23170
               OpBranch %20883
      %20883 = OpLabel
               OpControlBarrier %uint_2 %uint_2 %uint_264
      %15932 = OpINotEqual %bool %19243 %uint_0
               OpSelectionMerge %9094 None
               OpBranchConditional %15932 %15549 %9094
      %15549 = OpLabel
       %9637 = OpAccessChain %_ptr_Workgroup_uint %4017 %19824
       %6303 = OpAtomicOr %uint %9637 %uint_1 %uint_0 %23170
               OpBranch %9094
       %9094 = OpLabel
               OpControlBarrier %uint_2 %uint_2 %uint_264
               OpSelectionMerge %18021 None
               OpBranchConditional %15629 %20882 %18021
      %20882 = OpLabel
      %10747 = OpIMul %uint %15627 %uint_256
      %14186 = OpIAdd %uint %10747 %19824
      %14036 = OpAccessChain %_ptr_Workgroup_uint %4017 %19824
      %15421 = OpLoad %uint %14036
      %23477 = OpAccessChain %_ptr_Uniform_uint %5522 %int_0 %14186
               OpStore %23477 %15421
               OpBranch %18021
      %18021 = OpLabel
               OpBranch %18023
      %16414 = OpLabel
      %23178 = OpAccessChain %_ptr_Uniform_uint %3152 %int_0 %7572
      %12866 = OpLoad %uint %23178
      %12122 = OpBitwiseAnd %uint %18521 %uint_2
      %13988 = OpIMul %uint %12122 %uint_8
      %14462 = OpShiftRightLogical %uint %12866 %13988
      %15581 = OpBitwiseAnd %uint %14462 %uint_65535
      %19825 = OpShiftRightLogical %uint %8353 %uint_1
      %19244 = OpBitwiseAnd %uint %8353 %uint_1
      %14115 = OpIMul %uint %19244 %uint_16
      %23171 = OpShiftLeftLogical %uint %15581 %14115
      %15630 = OpIEqual %bool %19244 %uint_0
               OpSelectionMerge %20884 None
               OpBranchConditional %15630 %12149 %20884
      %12149 = OpLabel
      %21534 = OpAccessChain %_ptr_Workgroup_uint %4017 %19825
               OpStore %21534 %23171
               OpBranch %20884
      %20884 = OpLabel
               OpControlBarrier %uint_2 %uint_2 %uint_264
      %15933 = OpINotEqual %bool %19244 %uint_0
               OpSelectionMerge %9095 None
               OpBranchConditional %15933 %15550 %9095
      %15550 = OpLabel
       %9638 = OpAccessChain %_ptr_Workgroup_uint %4017 %19825
       %6304 = OpAtomicOr %uint %9638 %uint_1 %uint_0 %23171
               OpBranch %9095
       %9095 = OpLabel
               OpControlBarrier %uint_2 %uint_2 %uint_264
               OpSelectionMerge %18022 None
               OpBranchConditional %15630 %20885 %18022
      %20885 = OpLabel
      %10748 = OpIMul %uint %15627 %uint_512
      %14187 = OpIAdd %uint %10748 %19825
      %14037 = OpAccessChain %_ptr_Workgroup_uint %4017 %19825
      %15422 = OpLoad %uint %14037
      %23478 = OpAccessChain %_ptr_Uniform_uint %5522 %int_0 %14187
               OpStore %23478 %15422
               OpBranch %18022
      %18022 = OpLabel
               OpBranch %18023
      %16415 = OpLabel
      %20633 = OpAccessChain %_ptr_Uniform_uint %3152 %int_0 %7572
      %15646 = OpLoad %uint %20633
      %23479 = OpAccessChain %_ptr_Uniform_uint %5522 %int_0 %9085
               OpStore %23479 %15646
               OpBranch %18023
       %8158 = OpLabel
      %20634 = OpAccessChain %_ptr_Uniform_uint %3152 %int_0 %7572
      %15647 = OpLoad %uint %20634
      %21178 = OpAccessChain %_ptr_Uniform_uint %5522 %int_0 %9085
               OpStore %21178 %15647
      %19686 = OpIAdd %uint %9085 %uint_1
      %13384 = OpIAdd %uint %7572 %uint_1
      %10810 = OpAccessChain %_ptr_Uniform_uint %3152 %int_0 %13384
      %15423 = OpLoad %uint %10810
      %23480 = OpAccessChain %_ptr_Uniform_uint %5522 %int_0 %19686
               OpStore %23480 %15423
               OpBranch %18023
      %18023 = OpLabel
               OpBranch %14903
      %14903 = OpLabel
               OpReturn
               OpFunctionEnd
#endif

const uint32_t resolve_downscale_cs[] = {
    0x07230203, 0x00010000, 0x0008000B, 0x00006266, 0x00000000, 0x00020011, 0x00000001, 0x0006000B,
    0x00000001, 0x4C534C47, 0x6474732E, 0x3035342E, 0x00000000, 0x0003000E, 0x00000000, 0x00000001,
    0x0007000F, 0x00000005, 0x0000161F, 0x6E69616D, 0x00000000, 0x00000BEF, 0x00000DA3, 0x00060010,
    0x0000161F, 0x00000011, 0x00000020, 0x00000020, 0x00000001, 0x00040047, 0x00000BEF, 0x0000000B,
    0x0000001A, 0x00030047, 0x00000420, 0x00000002, 0x00050048, 0x00000420, 0x00000000, 0x00000023,
    0x00000000, 0x00050048, 0x00000420, 0x00000001, 0x00000023, 0x00000004, 0x00050048, 0x00000420,
    0x00000002, 0x00000023, 0x00000008, 0x00050048, 0x00000420, 0x00000003, 0x00000023, 0x0000000C,
    0x00050048, 0x00000420, 0x00000004, 0x00000023, 0x00000010, 0x00050048, 0x00000420, 0x00000005,
    0x00000023, 0x00000014, 0x00040047, 0x00000DA3, 0x0000000B, 0x0000001B, 0x00040047, 0x000007D0,
    0x00000006, 0x00000004, 0x00030047, 0x0000079C, 0x00000003, 0x00040048, 0x0000079C, 0x00000000,
    0x00000018, 0x00050048, 0x0000079C, 0x00000000, 0x00000023, 0x00000000, 0x00030047, 0x00000C50,
    0x00000018, 0x00040047, 0x00000C50, 0x00000021, 0x00000000, 0x00040047, 0x00000C50, 0x00000022,
    0x00000000, 0x00040047, 0x000007D1, 0x00000006, 0x00000004, 0x00030047, 0x0000079D, 0x00000003,
    0x00040048, 0x0000079D, 0x00000000, 0x00000019, 0x00050048, 0x0000079D, 0x00000000, 0x00000023,
    0x00000000, 0x00030047, 0x00001592, 0x00000019, 0x00040047, 0x00001592, 0x00000021, 0x00000001,
    0x00040047, 0x00001592, 0x00000022, 0x00000000, 0x00040047, 0x000000FC, 0x0000000B, 0x00000019,
    0x00020013, 0x00000008, 0x00030021, 0x00000502, 0x00000008, 0x00040015, 0x0000000B, 0x00000020,
    0x00000000, 0x00040017, 0x00000014, 0x0000000B, 0x00000003, 0x00040020, 0x00000291, 0x00000001,
    0x00000014, 0x0004003B, 0x00000291, 0x00000BEF, 0x00000001, 0x0004002B, 0x0000000B, 0x00000A0A,
    0x00000000, 0x00040020, 0x00000288, 0x00000001, 0x0000000B, 0x0008001E, 0x00000420, 0x0000000B,
    0x0000000B, 0x0000000B, 0x0000000B, 0x0000000B, 0x0000000B, 0x00040020, 0x0000069D, 0x00000009,
    0x00000420, 0x0004003B, 0x0000069D, 0x00001342, 0x00000009, 0x00040015, 0x0000000C, 0x00000020,
    0x00000001, 0x0004002B, 0x0000000C, 0x00000A14, 0x00000003, 0x00040020, 0x00000289, 0x00000009,
    0x0000000B, 0x00020014, 0x00000009, 0x0004003B, 0x00000291, 0x00000DA3, 0x00000001, 0x0004002B,
    0x0000000B, 0x00000A0D, 0x00000001, 0x0004002B, 0x0000000B, 0x00000A6A, 0x00000020, 0x0004002B,
    0x0000000C, 0x00000A11, 0x00000002, 0x0004002B, 0x0000000B, 0x00000A47, 0x00000400, 0x0004002B,
    0x0000000C, 0x00000A0B, 0x00000000, 0x0004002B, 0x0000000C, 0x00000A0E, 0x00000001, 0x0004002B,
    0x0000000C, 0x00000A1A, 0x00000005, 0x0004002B, 0x0000000C, 0x00000A17, 0x00000004, 0x0004002B,
    0x0000000B, 0x00000A10, 0x00000002, 0x0003001D, 0x000007D0, 0x0000000B, 0x0003001E, 0x0000079C,
    0x000007D0, 0x00040020, 0x00000A19, 0x00000002, 0x0000079C, 0x0004003B, 0x00000A19, 0x00000C50,
    0x00000002, 0x00040020, 0x0000028A, 0x00000002, 0x0000000B, 0x0004002B, 0x0000000B, 0x00000A13,
    0x00000003, 0x0004002B, 0x0000000B, 0x00000A22, 0x00000008, 0x0004002B, 0x0000000B, 0x00000144,
    0x000000FF, 0x0004001C, 0x00000293, 0x0000000B, 0x00000A47, 0x00040020, 0x00000510, 0x00000004,
    0x00000293, 0x0004003B, 0x00000510, 0x00000FB1, 0x00000004, 0x00040020, 0x0000028B, 0x00000004,
    0x0000000B, 0x0004002B, 0x0000000B, 0x0000015F, 0x00000108, 0x0003001D, 0x000007D1, 0x0000000B,
    0x0003001E, 0x0000079D, 0x000007D1, 0x00040020, 0x00000A1B, 0x00000002, 0x0000079D, 0x0004003B,
    0x00000A1B, 0x00001592, 0x00000002, 0x0004002B, 0x0000000B, 0x00000147, 0x00000100, 0x0004002B,
    0x0000000B, 0x000001C1, 0x0000FFFF, 0x0004002B, 0x0000000B, 0x00000A3A, 0x00000010, 0x0004002B,
    0x0000000B, 0x00000447, 0x00000200, 0x0006002C, 0x00000014, 0x000000FC, 0x00000A6A, 0x00000A6A,
    0x00000A0D, 0x00050036, 0x00000008, 0x0000161F, 0x00000000, 0x00000502, 0x000200F8, 0x00003B06,
    0x000300F7, 0x00003A37, 0x00000000, 0x000300FB, 0x00000A0A, 0x00002E68, 0x000200F8, 0x00002E68,
    0x00050041, 0x00000288, 0x000056E5, 0x00000BEF, 0x00000A0A, 0x0004003D, 0x0000000B, 0x00003D0B,
    0x000056E5, 0x00050041, 0x00000289, 0x000056D1, 0x00001342, 0x00000A14, 0x0004003D, 0x0000000B,
    0x00001BAD, 0x000056D1, 0x000500AE, 0x00000009, 0x00001CED, 0x00003D0B, 0x00001BAD, 0x000300F7,
    0x0000401D, 0x00000000, 0x000400FA, 0x00001CED, 0x000055E8, 0x0000401D, 0x000200F8, 0x000055E8,
    0x000200F9, 0x00003A37, 0x000200F8, 0x0000401D, 0x00050041, 0x00000288, 0x00005098, 0x00000DA3,
    0x00000A0D, 0x0004003D, 0x0000000B, 0x00003D0C, 0x00005098, 0x00050041, 0x00000288, 0x000053B3,
    0x00000DA3, 0x00000A0A, 0x0004003D, 0x0000000B, 0x00002F3A, 0x000053B3, 0x00050084, 0x0000000B,
    0x0000185A, 0x00003D0C, 0x00000A6A, 0x00050080, 0x0000000B, 0x000020A1, 0x0000185A, 0x00002F3A,
    0x00050041, 0x00000289, 0x0000408D, 0x00001342, 0x00000A11, 0x0004003D, 0x0000000B, 0x00003367,
    0x0000408D, 0x000500C4, 0x0000000B, 0x00003B4A, 0x00000A0D, 0x00003367, 0x00050084, 0x0000000B,
    0x00005537, 0x00000A47, 0x00003B4A, 0x00050041, 0x00000289, 0x000032C8, 0x00001342, 0x00000A0B,
    0x0004003D, 0x0000000B, 0x000048C4, 0x000032C8, 0x00050041, 0x00000289, 0x000053B4, 0x00001342,
    0x00000A0E, 0x0004003D, 0x0000000B, 0x00002F86, 0x000053B4, 0x00050084, 0x0000000B, 0x00006025,
    0x000048C4, 0x00002F86, 0x00050084, 0x0000000B, 0x0000359B, 0x00005537, 0x00006025, 0x00050041,
    0x00000289, 0x00002195, 0x00001342, 0x00000A1A, 0x0004003D, 0x0000000B, 0x000053DE, 0x00002195,
    0x000500AB, 0x00000009, 0x00005EF7, 0x000053DE, 0x00000A0A, 0x000500AC, 0x00000009, 0x00005C4D,
    0x00006025, 0x00000A0D, 0x000500A7, 0x00000009, 0x00002B1D, 0x00005EF7, 0x00005C4D, 0x000300F7,
    0x00005AE2, 0x00000000, 0x000400FA, 0x00002B1D, 0x00002C92, 0x00005AE2, 0x000200F8, 0x00002C92,
    0x000500C2, 0x0000000B, 0x00004EC7, 0x000048C4, 0x00000A0D, 0x000500C2, 0x0000000B, 0x000060CB,
    0x00002F86, 0x00000A0D, 0x00050084, 0x0000000B, 0x00003E8A, 0x000060CB, 0x000048C4, 0x00050080,
    0x0000000B, 0x00004B28, 0x00004EC7, 0x00003E8A, 0x000200F9, 0x00005AE2, 0x000200F8, 0x00005AE2,
    0x000700F5, 0x0000000B, 0x00004D24, 0x00000A0A, 0x0000401D, 0x00004B28, 0x00002C92, 0x00050041,
    0x00000289, 0x000061F2, 0x00001342, 0x00000A17, 0x0004003D, 0x0000000B, 0x00006265, 0x000061F2,
    0x00050084, 0x0000000B, 0x00001D53, 0x00003D0B, 0x0000359B, 0x00050080, 0x0000000B, 0x0000431E,
    0x00006265, 0x00001D53, 0x00050084, 0x0000000B, 0x00004CEC, 0x000020A1, 0x00003B4A, 0x00050084,
    0x0000000B, 0x00003F6E, 0x00004CEC, 0x00006025, 0x00050080, 0x0000000B, 0x00001AD5, 0x0000431E,
    0x00003F6E, 0x00050084, 0x0000000B, 0x00005486, 0x00004D24, 0x00003B4A, 0x00050080, 0x0000000B,
    0x00004859, 0x00001AD5, 0x00005486, 0x000500C2, 0x0000000B, 0x00001D94, 0x00004859, 0x00000A10,
    0x00050084, 0x0000000B, 0x00001B17, 0x00003D0B, 0x00005537, 0x00050080, 0x0000000B, 0x000025C1,
    0x00001B17, 0x00004CEC, 0x000500C2, 0x0000000B, 0x0000237D, 0x000025C1, 0x00000A10, 0x000300F7,
    0x00004667, 0x00000000, 0x000B00FB, 0x00003367, 0x00004667, 0x00000000, 0x00002E69, 0x00000001,
    0x0000401E, 0x00000002, 0x0000401F, 0x00000003, 0x00001FDE, 0x000200F8, 0x00002E69, 0x00060041,
    0x0000028A, 0x000060D7, 0x00000C50, 0x00000A0B, 0x00001D94, 0x0004003D, 0x0000000B, 0x00003241,
    0x000060D7, 0x000500C7, 0x0000000B, 0x00002F59, 0x00004859, 0x00000A13, 0x00050084, 0x0000000B,
    0x000036A3, 0x00002F59, 0x00000A22, 0x000500C2, 0x0000000B, 0x0000387D, 0x00003241, 0x000036A3,
    0x000500C7, 0x0000000B, 0x00003CDC, 0x0000387D, 0x00000144, 0x000500C2, 0x0000000B, 0x00004D70,
    0x000020A1, 0x00000A10, 0x000500C7, 0x0000000B, 0x00004B2B, 0x000020A1, 0x00000A13, 0x00050084,
    0x0000000B, 0x00003722, 0x00004B2B, 0x00000A22, 0x000500C4, 0x0000000B, 0x00005A82, 0x00003CDC,
    0x00003722, 0x000500AA, 0x00000009, 0x00003D0D, 0x00004B2B, 0x00000A0A, 0x000300F7, 0x00005193,
    0x00000000, 0x000400FA, 0x00003D0D, 0x00002F74, 0x00005193, 0x000200F8, 0x00002F74, 0x00050041,
    0x0000028B, 0x0000541D, 0x00000FB1, 0x00004D70, 0x0003003E, 0x0000541D, 0x00005A82, 0x000200F9,
    0x00005193, 0x000200F8, 0x00005193, 0x000400E0, 0x00000A10, 0x00000A10, 0x0000015F, 0x000500AB,
    0x00000009, 0x00003E3C, 0x00004B2B, 0x00000A0A, 0x000300F7, 0x00002386, 0x00000000, 0x000400FA,
    0x00003E3C, 0x00003CBD, 0x00002386, 0x000200F8, 0x00003CBD, 0x00050041, 0x0000028B, 0x000025A5,
    0x00000FB1, 0x00004D70, 0x000700F1, 0x0000000B, 0x0000189F, 0x000025A5, 0x00000A0D, 0x00000A0A,
    0x00005A82, 0x000200F9, 0x00002386, 0x000200F8, 0x00002386, 0x000400E0, 0x00000A10, 0x00000A10,
    0x0000015F, 0x000300F7, 0x00004665, 0x00000000, 0x000400FA, 0x00003D0D, 0x00005192, 0x00004665,
    0x000200F8, 0x00005192, 0x00050084, 0x0000000B, 0x000029FB, 0x00003D0B, 0x00000147, 0x00050080,
    0x0000000B, 0x0000376A, 0x000029FB, 0x00004D70, 0x00050041, 0x0000028B, 0x000036D4, 0x00000FB1,
    0x00004D70, 0x0004003D, 0x0000000B, 0x00003C3D, 0x000036D4, 0x00060041, 0x0000028A, 0x00005BB5,
    0x00001592, 0x00000A0B, 0x0000376A, 0x0003003E, 0x00005BB5, 0x00003C3D, 0x000200F9, 0x00004665,
    0x000200F8, 0x00004665, 0x000200F9, 0x00004667, 0x000200F8, 0x0000401E, 0x00060041, 0x0000028A,
    0x00005A8A, 0x00000C50, 0x00000A0B, 0x00001D94, 0x0004003D, 0x0000000B, 0x00003242, 0x00005A8A,
    0x000500C7, 0x0000000B, 0x00002F5A, 0x00004859, 0x00000A10, 0x00050084, 0x0000000B, 0x000036A4,
    0x00002F5A, 0x00000A22, 0x000500C2, 0x0000000B, 0x0000387E, 0x00003242, 0x000036A4, 0x000500C7,
    0x0000000B, 0x00003CDD, 0x0000387E, 0x000001C1, 0x000500C2, 0x0000000B, 0x00004D71, 0x000020A1,
    0x00000A0D, 0x000500C7, 0x0000000B, 0x00004B2C, 0x000020A1, 0x00000A0D, 0x00050084, 0x0000000B,
    0x00003723, 0x00004B2C, 0x00000A3A, 0x000500C4, 0x0000000B, 0x00005A83, 0x00003CDD, 0x00003723,
    0x000500AA, 0x00000009, 0x00003D0E, 0x00004B2C, 0x00000A0A, 0x000300F7, 0x00005194, 0x00000000,
    0x000400FA, 0x00003D0E, 0x00002F75, 0x00005194, 0x000200F8, 0x00002F75, 0x00050041, 0x0000028B,
    0x0000541E, 0x00000FB1, 0x00004D71, 0x0003003E, 0x0000541E, 0x00005A83, 0x000200F9, 0x00005194,
    0x000200F8, 0x00005194, 0x000400E0, 0x00000A10, 0x00000A10, 0x0000015F, 0x000500AB, 0x00000009,
    0x00003E3D, 0x00004B2C, 0x00000A0A, 0x000300F7, 0x00002387, 0x00000000, 0x000400FA, 0x00003E3D,
    0x00003CBE, 0x00002387, 0x000200F8, 0x00003CBE, 0x00050041, 0x0000028B, 0x000025A6, 0x00000FB1,
    0x00004D71, 0x000700F1, 0x0000000B, 0x000018A0, 0x000025A6, 0x00000A0D, 0x00000A0A, 0x00005A83,
    0x000200F9, 0x00002387, 0x000200F8, 0x00002387, 0x000400E0, 0x00000A10, 0x00000A10, 0x0000015F,
    0x000300F7, 0x00004666, 0x00000000, 0x000400FA, 0x00003D0E, 0x00005195, 0x00004666, 0x000200F8,
    0x00005195, 0x00050084, 0x0000000B, 0x000029FC, 0x00003D0B, 0x00000447, 0x00050080, 0x0000000B,
    0x0000376B, 0x000029FC, 0x00004D71, 0x00050041, 0x0000028B, 0x000036D5, 0x00000FB1, 0x00004D71,
    0x0004003D, 0x0000000B, 0x00003C3E, 0x000036D5, 0x00060041, 0x0000028A, 0x00005BB6, 0x00001592,
    0x00000A0B, 0x0000376B, 0x0003003E, 0x00005BB6, 0x00003C3E, 0x000200F9, 0x00004666, 0x000200F8,
    0x00004666, 0x000200F9, 0x00004667, 0x000200F8, 0x0000401F, 0x00060041, 0x0000028A, 0x00005099,
    0x00000C50, 0x00000A0B, 0x00001D94, 0x0004003D, 0x0000000B, 0x00003D1E, 0x00005099, 0x00060041,
    0x0000028A, 0x00005BB7, 0x00001592, 0x00000A0B, 0x0000237D, 0x0003003E, 0x00005BB7, 0x00003D1E,
    0x000200F9, 0x00004667, 0x000200F8, 0x00001FDE, 0x00060041, 0x0000028A, 0x0000509A, 0x00000C50,
    0x00000A0B, 0x00001D94, 0x0004003D, 0x0000000B, 0x00003D1F, 0x0000509A, 0x00060041, 0x0000028A,
    0x000052BA, 0x00001592, 0x00000A0B, 0x0000237D, 0x0003003E, 0x000052BA, 0x00003D1F, 0x00050080,
    0x0000000B, 0x00004CE6, 0x0000237D, 0x00000A0D, 0x00050080, 0x0000000B, 0x00003448, 0x00001D94,
    0x00000A0D, 0x00060041, 0x0000028A, 0x00002A3A, 0x00000C50, 0x00000A0B, 0x00003448, 0x0004003D,
    0x0000000B, 0x00003C3F, 0x00002A3A, 0x00060041, 0x0000028A, 0x00005BB8, 0x00001592, 0x00000A0B,
    0x00004CE6, 0x0003003E, 0x00005BB8, 0x00003C3F, 0x000200F9, 0x00004667, 0x000200F8, 0x00004667,
    0x000200F9, 0x00003A37, 0x000200F8, 0x00003A37, 0x000100FD, 0x00010038,
};
