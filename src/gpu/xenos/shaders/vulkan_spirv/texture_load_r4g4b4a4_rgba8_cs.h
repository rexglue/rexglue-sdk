// Generated with `spirv-as` from the R4G4B4A4 texture load shader.
#if 0
; SPIR-V
; Version: 1.0
; Generator: Khronos SPIR-V Tools Assembler; 0
; Bound: 307
; Schema: 0
               OpCapability Shader
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %2 "main" %gl_GlobalInvocationID
               OpExecutionMode %2 LocalSize 4 32 1
               OpDecorate %_struct_4 Block
               OpMemberDecorate %_struct_4 0 Offset 0
               OpMemberDecorate %_struct_4 1 Offset 4
               OpMemberDecorate %_struct_4 2 Offset 8
               OpMemberDecorate %_struct_4 3 Offset 12
               OpMemberDecorate %_struct_4 4 Offset 16
               OpMemberDecorate %_struct_4 5 Offset 28
               OpMemberDecorate %_struct_4 6 Offset 32
               OpMemberDecorate %_struct_4 7 Offset 36
               OpDecorate %gl_GlobalInvocationID BuiltIn GlobalInvocationId
               OpDecorate %_runtimearr_v4uint ArrayStride 16
               OpDecorate %_struct_6 BufferBlock
               OpMemberDecorate %_struct_6 0 NonWritable
               OpMemberDecorate %_struct_6 0 Offset 0
               OpDecorate %7 NonWritable
               OpDecorate %7 Binding 0
               OpDecorate %7 DescriptorSet 1
               OpDecorate %_runtimearr_v4uint_0 ArrayStride 16
               OpDecorate %_struct_9 BufferBlock
               OpMemberDecorate %_struct_9 0 NonReadable
               OpMemberDecorate %_struct_9 0 Offset 0
               OpDecorate %10 NonReadable
               OpDecorate %10 Binding 0
               OpDecorate %10 DescriptorSet 0
               OpDecorate %gl_WorkGroupSize BuiltIn WorkgroupSize
       %void = OpTypeVoid
         %13 = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %v4uint = OpTypeVector %uint 4
        %int = OpTypeInt 32 1
      %v2int = OpTypeVector %int 2
      %v3int = OpTypeVector %int 3
       %bool = OpTypeBool
     %v2uint = OpTypeVector %uint 2
     %v3uint = OpTypeVector %uint 3
         %22 = OpTypeFunction %v4uint %v4uint
%uint_268374015 = OpConstant %uint 268374015
     %uint_4 = OpConstant %uint 4
%uint_4026593280 = OpConstant %uint 4026593280
    %uint_12 = OpConstant %uint 12
     %uint_1 = OpConstant %uint 1
%uint_16711935 = OpConstant %uint 16711935
     %uint_8 = OpConstant %uint 8
%uint_4278255360 = OpConstant %uint 4278255360
     %uint_0 = OpConstant %uint 0
      %int_5 = OpConstant %int 5
     %uint_5 = OpConstant %uint 5
     %uint_7 = OpConstant %uint 7
      %int_7 = OpConstant %int 7
     %int_14 = OpConstant %int 14
      %int_2 = OpConstant %int 2
    %int_n16 = OpConstant %int -16
      %int_1 = OpConstant %int 1
     %int_15 = OpConstant %int 15
      %int_4 = OpConstant %int 4
   %int_n512 = OpConstant %int -512
      %int_3 = OpConstant %int 3
     %int_16 = OpConstant %int 16
    %int_448 = OpConstant %int 448
      %int_8 = OpConstant %int 8
      %int_6 = OpConstant %int 6
     %int_63 = OpConstant %int 63
     %uint_2 = OpConstant %uint 2
%int_268435455 = OpConstant %int 268435455
     %int_n2 = OpConstant %int -2
     %uint_3 = OpConstant %uint 3
    %uint_32 = OpConstant %uint 32
    %uint_64 = OpConstant %uint 64
  %_struct_4 = OpTypeStruct %uint %uint %uint %uint %v3uint %uint %uint %uint
%_ptr_PushConstant__struct_4 = OpTypePointer PushConstant %_struct_4
         %56 = OpVariable %_ptr_PushConstant__struct_4 PushConstant
      %int_0 = OpConstant %int 0
%_ptr_PushConstant_uint = OpTypePointer PushConstant %uint
%_ptr_PushConstant_v3uint = OpTypePointer PushConstant %v3uint
%_ptr_Input_v3uint = OpTypePointer Input %v3uint
%gl_GlobalInvocationID = OpVariable %_ptr_Input_v3uint Input
         %61 = OpConstantComposite %v3uint %uint_4 %uint_0 %uint_0
     %v2bool = OpTypeVector %bool 2
%_runtimearr_v4uint = OpTypeRuntimeArray %v4uint
  %_struct_6 = OpTypeStruct %_runtimearr_v4uint
%_ptr_Uniform__struct_6 = OpTypePointer Uniform %_struct_6
          %7 = OpVariable %_ptr_Uniform__struct_6 Uniform
%_ptr_Uniform_v4uint = OpTypePointer Uniform %v4uint
%_runtimearr_v4uint_0 = OpTypeRuntimeArray %v4uint
  %_struct_9 = OpTypeStruct %_runtimearr_v4uint_0
%_ptr_Uniform__struct_9 = OpTypePointer Uniform %_struct_9
         %10 = OpVariable %_ptr_Uniform__struct_9 Uniform
%gl_WorkGroupSize = OpConstantComposite %v3uint %uint_4 %uint_32 %uint_1
         %66 = OpConstantComposite %v4uint %uint_16711935 %uint_16711935 %uint_16711935 %uint_16711935
         %67 = OpConstantComposite %v4uint %uint_8 %uint_8 %uint_8 %uint_8
         %68 = OpConstantComposite %v4uint %uint_4278255360 %uint_4278255360 %uint_4278255360 %uint_4278255360
         %69 = OpConstantComposite %v4uint %uint_268374015 %uint_268374015 %uint_268374015 %uint_268374015
         %70 = OpConstantComposite %v4uint %uint_4 %uint_4 %uint_4 %uint_4
         %71 = OpConstantComposite %v4uint %uint_4026593280 %uint_4026593280 %uint_4026593280 %uint_4026593280
         %72 = OpConstantComposite %v4uint %uint_12 %uint_12 %uint_12 %uint_12
    %uint_16 = OpConstant %uint 16
    %uint_15 = OpConstant %uint 15
    %uint_24 = OpConstant %uint 24
 %uint_65535 = OpConstant %uint 65535
  %uint_3840 = OpConstant %uint 3840
   %uint_240 = OpConstant %uint 240
 %uint_61440 = OpConstant %uint 61440
         %80 = OpConstantComposite %v4uint %uint_65535 %uint_65535 %uint_65535 %uint_65535
         %81 = OpConstantComposite %v4uint %uint_16 %uint_16 %uint_16 %uint_16
         %82 = OpConstantComposite %v4uint %uint_15 %uint_15 %uint_15 %uint_15
         %83 = OpConstantComposite %v4uint %uint_24 %uint_24 %uint_24 %uint_24
         %84 = OpConstantComposite %v4uint %uint_3840 %uint_3840 %uint_3840 %uint_3840
         %85 = OpConstantComposite %v4uint %uint_240 %uint_240 %uint_240 %uint_240
         %86 = OpConstantComposite %v4uint %uint_61440 %uint_61440 %uint_61440 %uint_61440
         %87 = OpFunction %v4uint None %22
         %88 = OpFunctionParameter %v4uint
         %89 = OpLabel
         %90 = OpBitwiseAnd %v4uint %88 %82
         %91 = OpBitwiseAnd %v4uint %88 %85
         %92 = OpShiftLeftLogical %v4uint %91 %70
         %93 = OpBitwiseAnd %v4uint %88 %84
         %94 = OpShiftLeftLogical %v4uint %93 %67
         %95 = OpBitwiseAnd %v4uint %88 %86
         %96 = OpShiftLeftLogical %v4uint %95 %72
         %97 = OpBitwiseOr %v4uint %90 %92
         %98 = OpBitwiseOr %v4uint %94 %96
         %99 = OpBitwiseOr %v4uint %97 %98
        %100 = OpShiftLeftLogical %v4uint %99 %70
        %101 = OpBitwiseOr %v4uint %99 %100
               OpReturnValue %101
               OpFunctionEnd
          %2 = OpFunction %void None %13
        %102 = OpLabel
               OpSelectionMerge %103 None
               OpSwitch %uint_0 %104
        %104 = OpLabel
        %105 = OpAccessChain %_ptr_PushConstant_uint %56 %int_0
        %106 = OpLoad %uint %105
        %107 = OpBitwiseAnd %uint %106 %uint_1
        %108 = OpINotEqual %bool %107 %uint_0
        %109 = OpBitwiseAnd %uint %106 %uint_2
        %110 = OpINotEqual %bool %109 %uint_0
        %111 = OpShiftRightLogical %uint %106 %uint_2
        %112 = OpBitwiseAnd %uint %111 %uint_3
        %113 = OpAccessChain %_ptr_PushConstant_uint %56 %int_1
        %114 = OpLoad %uint %113
        %115 = OpAccessChain %_ptr_PushConstant_uint %56 %int_2
        %116 = OpLoad %uint %115
        %117 = OpAccessChain %_ptr_PushConstant_uint %56 %int_3
        %118 = OpLoad %uint %117
        %119 = OpAccessChain %_ptr_PushConstant_v3uint %56 %int_4
        %120 = OpLoad %v3uint %119
        %121 = OpAccessChain %_ptr_PushConstant_uint %56 %int_5
        %122 = OpLoad %uint %121
        %123 = OpAccessChain %_ptr_PushConstant_uint %56 %int_6
        %124 = OpLoad %uint %123
        %125 = OpLoad %v3uint %gl_GlobalInvocationID
        %126 = OpShiftLeftLogical %v3uint %125 %61
        %127 = OpVectorShuffle %v2uint %126 %126 0 1
        %128 = OpVectorShuffle %v2uint %120 %120 0 1
        %129 = OpUGreaterThanEqual %v2bool %127 %128
        %130 = OpAny %bool %129
               OpSelectionMerge %131 DontFlatten
               OpBranchConditional %130 %132 %131
        %132 = OpLabel
               OpBranch %103
        %131 = OpLabel
        %133 = OpBitcast %v3int %126
        %134 = OpCompositeExtract %uint %120 1
        %135 = OpCompositeExtract %int %133 0
        %136 = OpIMul %int %135 %int_2
        %137 = OpIMul %int %135 %int_4
        %138 = OpCompositeExtract %int %133 2
        %139 = OpBitcast %int %134
        %140 = OpIMul %int %138 %139
        %141 = OpCompositeExtract %int %133 1
        %142 = OpIAdd %int %140 %141
        %143 = OpBitcast %int %124
        %144 = OpIMul %int %142 %143
        %145 = OpIAdd %int %137 %144
        %146 = OpBitcast %uint %145
        %147 = OpIAdd %uint %146 %122
        %148 = OpShiftRightLogical %uint %147 %uint_4
               OpSelectionMerge %149 DontFlatten
               OpBranchConditional %108 %150 %151
        %150 = OpLabel
               OpSelectionMerge %152 DontFlatten
               OpBranchConditional %110 %153 %154
        %153 = OpLabel
        %155 = OpShiftRightArithmetic %int %141 %int_4
        %156 = OpShiftRightArithmetic %int %138 %int_2
        %157 = OpShiftRightLogical %uint %118 %uint_4
        %158 = OpBitcast %int %157
        %159 = OpIMul %int %156 %158
        %160 = OpIAdd %int %155 %159
        %161 = OpShiftRightLogical %uint %116 %uint_5
        %162 = OpBitcast %int %161
        %163 = OpIMul %int %160 %162
        %164 = OpShiftRightArithmetic %int %135 %int_5
        %165 = OpIAdd %int %164 %163
        %166 = OpShiftLeftLogical %int %165 %uint_7
        %167 = OpBitwiseAnd %int %166 %int_268435455
        %168 = OpShiftLeftLogical %int %167 %int_1
        %169 = OpBitwiseAnd %int %135 %int_7
        %170 = OpBitwiseAnd %int %141 %int_6
        %171 = OpShiftLeftLogical %int %170 %int_2
        %172 = OpIAdd %int %169 %171
        %173 = OpShiftLeftLogical %int %172 %uint_7
        %174 = OpShiftRightArithmetic %int %173 %int_6
        %175 = OpShiftRightArithmetic %int %141 %int_3
        %176 = OpIAdd %int %175 %156
        %177 = OpBitwiseAnd %int %176 %int_1
        %178 = OpShiftRightArithmetic %int %135 %int_3
        %179 = OpShiftLeftLogical %int %177 %int_1
        %180 = OpIAdd %int %178 %179
        %181 = OpBitwiseAnd %int %180 %int_3
        %182 = OpShiftLeftLogical %int %181 %int_1
        %183 = OpIAdd %int %177 %182
        %184 = OpBitwiseAnd %int %174 %int_n16
        %185 = OpIAdd %int %168 %184
        %186 = OpShiftLeftLogical %int %185 %int_1
        %187 = OpBitwiseAnd %int %174 %int_15
        %188 = OpIAdd %int %186 %187
        %189 = OpBitwiseAnd %int %138 %int_3
        %190 = OpShiftLeftLogical %int %189 %uint_7
        %191 = OpIAdd %int %188 %190
        %192 = OpBitwiseAnd %int %141 %int_1
        %193 = OpShiftLeftLogical %int %192 %int_4
        %194 = OpIAdd %int %191 %193
        %195 = OpBitwiseAnd %int %183 %int_1
        %196 = OpShiftLeftLogical %int %195 %int_3
        %197 = OpShiftRightArithmetic %int %194 %int_6
        %198 = OpBitwiseAnd %int %197 %int_7
        %199 = OpIAdd %int %196 %198
        %200 = OpShiftLeftLogical %int %199 %int_3
        %201 = OpBitwiseAnd %int %183 %int_n2
        %202 = OpIAdd %int %200 %201
        %203 = OpShiftLeftLogical %int %202 %int_2
        %204 = OpBitwiseAnd %int %194 %int_n512
        %205 = OpIAdd %int %203 %204
        %206 = OpShiftLeftLogical %int %205 %int_3
        %207 = OpBitwiseAnd %int %194 %int_63
        %208 = OpIAdd %int %206 %207
               OpBranch %152
        %154 = OpLabel
        %209 = OpBitcast %v2int %127
        %210 = OpCompositeExtract %int %209 0
        %211 = OpShiftRightArithmetic %int %210 %int_5
        %212 = OpCompositeExtract %int %209 1
        %213 = OpShiftRightArithmetic %int %212 %int_5
        %214 = OpShiftRightLogical %uint %116 %uint_5
        %215 = OpBitcast %int %214
        %216 = OpIMul %int %213 %215
        %217 = OpIAdd %int %211 %216
        %218 = OpShiftLeftLogical %int %217 %uint_8
        %219 = OpBitwiseAnd %int %210 %int_7
        %220 = OpBitwiseAnd %int %212 %int_14
        %221 = OpShiftLeftLogical %int %220 %int_2
        %222 = OpIAdd %int %219 %221
        %223 = OpShiftLeftLogical %int %222 %uint_1
        %224 = OpBitwiseAnd %int %223 %int_n16
        %225 = OpShiftLeftLogical %int %224 %int_1
        %226 = OpIAdd %int %218 %225
        %227 = OpBitwiseAnd %int %223 %int_15
        %228 = OpIAdd %int %226 %227
        %229 = OpBitwiseAnd %int %212 %int_1
        %230 = OpShiftLeftLogical %int %229 %int_4
        %231 = OpIAdd %int %228 %230
        %232 = OpBitwiseAnd %int %231 %int_n512
        %233 = OpShiftLeftLogical %int %232 %int_3
        %234 = OpBitwiseAnd %int %212 %int_16
        %235 = OpShiftLeftLogical %int %234 %int_7
        %236 = OpIAdd %int %233 %235
        %237 = OpBitwiseAnd %int %231 %int_448
        %238 = OpShiftLeftLogical %int %237 %int_2
        %239 = OpIAdd %int %236 %238
        %240 = OpBitwiseAnd %int %212 %int_8
        %241 = OpShiftRightArithmetic %int %240 %int_2
        %242 = OpShiftRightArithmetic %int %210 %int_3
        %243 = OpIAdd %int %241 %242
        %244 = OpBitwiseAnd %int %243 %int_3
        %245 = OpShiftLeftLogical %int %244 %int_6
        %246 = OpIAdd %int %239 %245
        %247 = OpBitwiseAnd %int %231 %int_63
        %248 = OpIAdd %int %246 %247
               OpBranch %152
        %152 = OpLabel
        %249 = OpPhi %int %208 %153 %248 %154
               OpBranch %149
        %151 = OpLabel
        %250 = OpBitcast %int %118
        %251 = OpIMul %int %138 %250
        %252 = OpIAdd %int %251 %141
        %253 = OpBitcast %int %116
        %254 = OpIMul %int %252 %253
        %255 = OpIAdd %int %136 %254
               OpBranch %149
        %149 = OpLabel
        %256 = OpPhi %int %249 %152 %255 %151
        %257 = OpBitcast %int %114
        %258 = OpIAdd %int %257 %256
        %259 = OpBitcast %uint %258
        %260 = OpShiftRightLogical %uint %259 %uint_4
        %261 = OpAccessChain %_ptr_Uniform_v4uint %7 %int_0 %260
        %262 = OpLoad %v4uint %261
        %263 = OpIEqual %bool %112 %uint_1
               OpSelectionMerge %264 None
               OpBranchConditional %263 %265 %264
        %265 = OpLabel
        %266 = OpBitwiseAnd %v4uint %262 %66
        %267 = OpShiftLeftLogical %v4uint %266 %67
        %268 = OpBitwiseAnd %v4uint %262 %68
        %269 = OpShiftRightLogical %v4uint %268 %67
        %270 = OpBitwiseOr %v4uint %267 %269
               OpBranch %264
        %264 = OpLabel
        %271 = OpPhi %v4uint %262 %149 %270 %265
        %272 = OpBitwiseAnd %v4uint %271 %80
        %273 = OpShiftRightLogical %v4uint %271 %81
        %274 = OpVectorShuffle %v4uint %272 %273 0 4 1 5
        %275 = OpVectorShuffle %v4uint %272 %273 2 6 3 7
        %276 = OpFunctionCall %v4uint %87 %274
        %277 = OpFunctionCall %v4uint %87 %275
        %278 = OpAccessChain %_ptr_Uniform_v4uint %10 %int_0 %148
               OpStore %278 %276
        %279 = OpIAdd %uint %148 %int_1
        %280 = OpAccessChain %_ptr_Uniform_v4uint %10 %int_0 %279
               OpStore %280 %277
               OpSelectionMerge %281 DontFlatten
               OpBranchConditional %108 %282 %283
        %282 = OpLabel
               OpBranch %281
        %283 = OpLabel
               OpBranch %281
        %281 = OpLabel
        %284 = OpPhi %uint %uint_64 %282 %uint_16 %283
        %285 = OpShiftRightLogical %uint %284 %uint_4
        %286 = OpIAdd %uint %260 %285
        %287 = OpAccessChain %_ptr_Uniform_v4uint %7 %int_0 %286
        %288 = OpLoad %v4uint %287
               OpSelectionMerge %289 None
               OpBranchConditional %263 %290 %289
        %290 = OpLabel
        %291 = OpBitwiseAnd %v4uint %288 %66
        %292 = OpShiftLeftLogical %v4uint %291 %67
        %293 = OpBitwiseAnd %v4uint %288 %68
        %294 = OpShiftRightLogical %v4uint %293 %67
        %295 = OpBitwiseOr %v4uint %292 %294
               OpBranch %289
        %289 = OpLabel
        %296 = OpPhi %v4uint %288 %281 %295 %290
        %297 = OpBitwiseAnd %v4uint %296 %80
        %298 = OpShiftRightLogical %v4uint %296 %81
        %299 = OpVectorShuffle %v4uint %297 %298 0 4 1 5
        %300 = OpVectorShuffle %v4uint %297 %298 2 6 3 7
        %301 = OpFunctionCall %v4uint %87 %299
        %302 = OpFunctionCall %v4uint %87 %300
        %303 = OpIAdd %uint %279 %int_1
        %304 = OpAccessChain %_ptr_Uniform_v4uint %10 %int_0 %303
               OpStore %304 %301
        %305 = OpIAdd %uint %303 %int_1
        %306 = OpAccessChain %_ptr_Uniform_v4uint %10 %int_0 %305
               OpStore %306 %302
               OpBranch %103
        %103 = OpLabel
               OpReturn
               OpFunctionEnd
#endif

const uint32_t texture_load_r4g4b4a4_rgba8_cs[] = {
    0x07230203, 0x00010000, 0x00070000, 0x00000133, 0x00000000, 0x00020011, 0x00000001, 0x0006000B,
    0x00000001, 0x4C534C47, 0x6474732E, 0x3035342E, 0x00000000, 0x0003000E, 0x00000000, 0x00000001,
    0x0006000F, 0x00000005, 0x00000002, 0x6E69616D, 0x00000000, 0x00000003, 0x00060010, 0x00000002,
    0x00000011, 0x00000004, 0x00000020, 0x00000001, 0x00030047, 0x00000004, 0x00000002, 0x00050048,
    0x00000004, 0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x00000004, 0x00000001, 0x00000023,
    0x00000004, 0x00050048, 0x00000004, 0x00000002, 0x00000023, 0x00000008, 0x00050048, 0x00000004,
    0x00000003, 0x00000023, 0x0000000C, 0x00050048, 0x00000004, 0x00000004, 0x00000023, 0x00000010,
    0x00050048, 0x00000004, 0x00000005, 0x00000023, 0x0000001C, 0x00050048, 0x00000004, 0x00000006,
    0x00000023, 0x00000020, 0x00050048, 0x00000004, 0x00000007, 0x00000023, 0x00000024, 0x00040047,
    0x00000003, 0x0000000B, 0x0000001C, 0x00040047, 0x00000005, 0x00000006, 0x00000010, 0x00030047,
    0x00000006, 0x00000003, 0x00040048, 0x00000006, 0x00000000, 0x00000018, 0x00050048, 0x00000006,
    0x00000000, 0x00000023, 0x00000000, 0x00030047, 0x00000007, 0x00000018, 0x00040047, 0x00000007,
    0x00000021, 0x00000000, 0x00040047, 0x00000007, 0x00000022, 0x00000001, 0x00040047, 0x00000008,
    0x00000006, 0x00000010, 0x00030047, 0x00000009, 0x00000003, 0x00040048, 0x00000009, 0x00000000,
    0x00000019, 0x00050048, 0x00000009, 0x00000000, 0x00000023, 0x00000000, 0x00030047, 0x0000000A,
    0x00000019, 0x00040047, 0x0000000A, 0x00000021, 0x00000000, 0x00040047, 0x0000000A, 0x00000022,
    0x00000000, 0x00040047, 0x0000000B, 0x0000000B, 0x00000019, 0x00020013, 0x0000000C, 0x00030021,
    0x0000000D, 0x0000000C, 0x00040015, 0x0000000E, 0x00000020, 0x00000000, 0x00040017, 0x0000000F,
    0x0000000E, 0x00000004, 0x00040015, 0x00000010, 0x00000020, 0x00000001, 0x00040017, 0x00000011,
    0x00000010, 0x00000002, 0x00040017, 0x00000012, 0x00000010, 0x00000003, 0x00020014, 0x00000013,
    0x00040017, 0x00000014, 0x0000000E, 0x00000002, 0x00040017, 0x00000015, 0x0000000E, 0x00000003,
    0x00040021, 0x00000016, 0x0000000F, 0x0000000F, 0x0004002B, 0x0000000E, 0x00000017, 0x0FFF0FFF,
    0x0004002B, 0x0000000E, 0x00000018, 0x00000004, 0x0004002B, 0x0000000E, 0x00000019, 0xF000F000,
    0x0004002B, 0x0000000E, 0x0000001A, 0x0000000C, 0x0004002B, 0x0000000E, 0x0000001B, 0x00000001,
    0x0004002B, 0x0000000E, 0x0000001C, 0x00FF00FF, 0x0004002B, 0x0000000E, 0x0000001D, 0x00000008,
    0x0004002B, 0x0000000E, 0x0000001E, 0xFF00FF00, 0x0004002B, 0x0000000E, 0x0000001F, 0x00000000,
    0x0004002B, 0x00000010, 0x00000020, 0x00000005, 0x0004002B, 0x0000000E, 0x00000021, 0x00000005,
    0x0004002B, 0x0000000E, 0x00000022, 0x00000007, 0x0004002B, 0x00000010, 0x00000023, 0x00000007,
    0x0004002B, 0x00000010, 0x00000024, 0x0000000E, 0x0004002B, 0x00000010, 0x00000025, 0x00000002,
    0x0004002B, 0x00000010, 0x00000026, 0xFFFFFFF0, 0x0004002B, 0x00000010, 0x00000027, 0x00000001,
    0x0004002B, 0x00000010, 0x00000028, 0x0000000F, 0x0004002B, 0x00000010, 0x00000029, 0x00000004,
    0x0004002B, 0x00000010, 0x0000002A, 0xFFFFFE00, 0x0004002B, 0x00000010, 0x0000002B, 0x00000003,
    0x0004002B, 0x00000010, 0x0000002C, 0x00000010, 0x0004002B, 0x00000010, 0x0000002D, 0x000001C0,
    0x0004002B, 0x00000010, 0x0000002E, 0x00000008, 0x0004002B, 0x00000010, 0x0000002F, 0x00000006,
    0x0004002B, 0x00000010, 0x00000030, 0x0000003F, 0x0004002B, 0x0000000E, 0x00000031, 0x00000002,
    0x0004002B, 0x00000010, 0x00000032, 0x0FFFFFFF, 0x0004002B, 0x00000010, 0x00000033, 0xFFFFFFFE,
    0x0004002B, 0x0000000E, 0x00000034, 0x00000003, 0x0004002B, 0x0000000E, 0x00000035, 0x00000020,
    0x0004002B, 0x0000000E, 0x00000036, 0x00000040, 0x000A001E, 0x00000004, 0x0000000E, 0x0000000E,
    0x0000000E, 0x0000000E, 0x00000015, 0x0000000E, 0x0000000E, 0x0000000E, 0x00040020, 0x00000037,
    0x00000009, 0x00000004, 0x0004003B, 0x00000037, 0x00000038, 0x00000009, 0x0004002B, 0x00000010,
    0x00000039, 0x00000000, 0x00040020, 0x0000003A, 0x00000009, 0x0000000E, 0x00040020, 0x0000003B,
    0x00000009, 0x00000015, 0x00040020, 0x0000003C, 0x00000001, 0x00000015, 0x0004003B, 0x0000003C,
    0x00000003, 0x00000001, 0x0006002C, 0x00000015, 0x0000003D, 0x00000018, 0x0000001F, 0x0000001F,
    0x00040017, 0x0000003E, 0x00000013, 0x00000002, 0x0003001D, 0x00000005, 0x0000000F, 0x0003001E,
    0x00000006, 0x00000005, 0x00040020, 0x0000003F, 0x00000002, 0x00000006, 0x0004003B, 0x0000003F,
    0x00000007, 0x00000002, 0x00040020, 0x00000040, 0x00000002, 0x0000000F, 0x0003001D, 0x00000008,
    0x0000000F, 0x0003001E, 0x00000009, 0x00000008, 0x00040020, 0x00000041, 0x00000002, 0x00000009,
    0x0004003B, 0x00000041, 0x0000000A, 0x00000002, 0x0006002C, 0x00000015, 0x0000000B, 0x00000018,
    0x00000035, 0x0000001B, 0x0007002C, 0x0000000F, 0x00000042, 0x0000001C, 0x0000001C, 0x0000001C,
    0x0000001C, 0x0007002C, 0x0000000F, 0x00000043, 0x0000001D, 0x0000001D, 0x0000001D, 0x0000001D,
    0x0007002C, 0x0000000F, 0x00000044, 0x0000001E, 0x0000001E, 0x0000001E, 0x0000001E, 0x0007002C,
    0x0000000F, 0x00000045, 0x00000017, 0x00000017, 0x00000017, 0x00000017, 0x0007002C, 0x0000000F,
    0x00000046, 0x00000018, 0x00000018, 0x00000018, 0x00000018, 0x0007002C, 0x0000000F, 0x00000047,
    0x00000019, 0x00000019, 0x00000019, 0x00000019, 0x0007002C, 0x0000000F, 0x00000048, 0x0000001A,
    0x0000001A, 0x0000001A, 0x0000001A, 0x0004002B, 0x0000000E, 0x00000049, 0x00000010, 0x0004002B,
    0x0000000E, 0x0000004A, 0x0000000F, 0x0004002B, 0x0000000E, 0x0000004B, 0x00000018, 0x0004002B,
    0x0000000E, 0x0000004C, 0x0000FFFF, 0x0004002B, 0x0000000E, 0x0000004D, 0x00000F00, 0x0004002B,
    0x0000000E, 0x0000004E, 0x000000F0, 0x0004002B, 0x0000000E, 0x0000004F, 0x0000F000, 0x0007002C,
    0x0000000F, 0x00000050, 0x0000004C, 0x0000004C, 0x0000004C, 0x0000004C, 0x0007002C, 0x0000000F,
    0x00000051, 0x00000049, 0x00000049, 0x00000049, 0x00000049, 0x0007002C, 0x0000000F, 0x00000052,
    0x0000004A, 0x0000004A, 0x0000004A, 0x0000004A, 0x0007002C, 0x0000000F, 0x00000053, 0x0000004B,
    0x0000004B, 0x0000004B, 0x0000004B, 0x0007002C, 0x0000000F, 0x00000054, 0x0000004D, 0x0000004D,
    0x0000004D, 0x0000004D, 0x0007002C, 0x0000000F, 0x00000055, 0x0000004E, 0x0000004E, 0x0000004E,
    0x0000004E, 0x0007002C, 0x0000000F, 0x00000056, 0x0000004F, 0x0000004F, 0x0000004F, 0x0000004F,
    0x00050036, 0x0000000F, 0x00000057, 0x00000000, 0x00000016, 0x00030037, 0x0000000F, 0x00000058,
    0x000200F8, 0x00000059, 0x000500C7, 0x0000000F, 0x0000005A, 0x00000058, 0x00000052, 0x000500C7,
    0x0000000F, 0x0000005B, 0x00000058, 0x00000055, 0x000500C4, 0x0000000F, 0x0000005C, 0x0000005B,
    0x00000046, 0x000500C7, 0x0000000F, 0x0000005D, 0x00000058, 0x00000054, 0x000500C4, 0x0000000F,
    0x0000005E, 0x0000005D, 0x00000043, 0x000500C7, 0x0000000F, 0x0000005F, 0x00000058, 0x00000056,
    0x000500C4, 0x0000000F, 0x00000060, 0x0000005F, 0x00000048, 0x000500C5, 0x0000000F, 0x00000061,
    0x0000005A, 0x0000005C, 0x000500C5, 0x0000000F, 0x00000062, 0x0000005E, 0x00000060, 0x000500C5,
    0x0000000F, 0x00000063, 0x00000061, 0x00000062, 0x000500C4, 0x0000000F, 0x00000064, 0x00000063,
    0x00000046, 0x000500C5, 0x0000000F, 0x00000065, 0x00000063, 0x00000064, 0x000200FE, 0x00000065,
    0x00010038, 0x00050036, 0x0000000C, 0x00000002, 0x00000000, 0x0000000D, 0x000200F8, 0x00000066,
    0x000300F7, 0x00000067, 0x00000000, 0x000300FB, 0x0000001F, 0x00000068, 0x000200F8, 0x00000068,
    0x00050041, 0x0000003A, 0x00000069, 0x00000038, 0x00000039, 0x0004003D, 0x0000000E, 0x0000006A,
    0x00000069, 0x000500C7, 0x0000000E, 0x0000006B, 0x0000006A, 0x0000001B, 0x000500AB, 0x00000013,
    0x0000006C, 0x0000006B, 0x0000001F, 0x000500C7, 0x0000000E, 0x0000006D, 0x0000006A, 0x00000031,
    0x000500AB, 0x00000013, 0x0000006E, 0x0000006D, 0x0000001F, 0x000500C2, 0x0000000E, 0x0000006F,
    0x0000006A, 0x00000031, 0x000500C7, 0x0000000E, 0x00000070, 0x0000006F, 0x00000034, 0x00050041,
    0x0000003A, 0x00000071, 0x00000038, 0x00000027, 0x0004003D, 0x0000000E, 0x00000072, 0x00000071,
    0x00050041, 0x0000003A, 0x00000073, 0x00000038, 0x00000025, 0x0004003D, 0x0000000E, 0x00000074,
    0x00000073, 0x00050041, 0x0000003A, 0x00000075, 0x00000038, 0x0000002B, 0x0004003D, 0x0000000E,
    0x00000076, 0x00000075, 0x00050041, 0x0000003B, 0x00000077, 0x00000038, 0x00000029, 0x0004003D,
    0x00000015, 0x00000078, 0x00000077, 0x00050041, 0x0000003A, 0x00000079, 0x00000038, 0x00000020,
    0x0004003D, 0x0000000E, 0x0000007A, 0x00000079, 0x00050041, 0x0000003A, 0x0000007B, 0x00000038,
    0x0000002F, 0x0004003D, 0x0000000E, 0x0000007C, 0x0000007B, 0x0004003D, 0x00000015, 0x0000007D,
    0x00000003, 0x000500C4, 0x00000015, 0x0000007E, 0x0000007D, 0x0000003D, 0x0007004F, 0x00000014,
    0x0000007F, 0x0000007E, 0x0000007E, 0x00000000, 0x00000001, 0x0007004F, 0x00000014, 0x00000080,
    0x00000078, 0x00000078, 0x00000000, 0x00000001, 0x000500AE, 0x0000003E, 0x00000081, 0x0000007F,
    0x00000080, 0x0004009A, 0x00000013, 0x00000082, 0x00000081, 0x000300F7, 0x00000083, 0x00000002,
    0x000400FA, 0x00000082, 0x00000084, 0x00000083, 0x000200F8, 0x00000084, 0x000200F9, 0x00000067,
    0x000200F8, 0x00000083, 0x0004007C, 0x00000012, 0x00000085, 0x0000007E, 0x00050051, 0x0000000E,
    0x00000086, 0x00000078, 0x00000001, 0x00050051, 0x00000010, 0x00000087, 0x00000085, 0x00000000,
    0x00050084, 0x00000010, 0x00000088, 0x00000087, 0x00000025, 0x00050084, 0x00000010, 0x00000089,
    0x00000087, 0x00000029, 0x00050051, 0x00000010, 0x0000008A, 0x00000085, 0x00000002, 0x0004007C,
    0x00000010, 0x0000008B, 0x00000086, 0x00050084, 0x00000010, 0x0000008C, 0x0000008A, 0x0000008B,
    0x00050051, 0x00000010, 0x0000008D, 0x00000085, 0x00000001, 0x00050080, 0x00000010, 0x0000008E,
    0x0000008C, 0x0000008D, 0x0004007C, 0x00000010, 0x0000008F, 0x0000007C, 0x00050084, 0x00000010,
    0x00000090, 0x0000008E, 0x0000008F, 0x00050080, 0x00000010, 0x00000091, 0x00000089, 0x00000090,
    0x0004007C, 0x0000000E, 0x00000092, 0x00000091, 0x00050080, 0x0000000E, 0x00000093, 0x00000092,
    0x0000007A, 0x000500C2, 0x0000000E, 0x00000094, 0x00000093, 0x00000018, 0x000300F7, 0x00000095,
    0x00000002, 0x000400FA, 0x0000006C, 0x00000096, 0x00000097, 0x000200F8, 0x00000096, 0x000300F7,
    0x00000098, 0x00000002, 0x000400FA, 0x0000006E, 0x00000099, 0x0000009A, 0x000200F8, 0x00000099,
    0x000500C3, 0x00000010, 0x0000009B, 0x0000008D, 0x00000029, 0x000500C3, 0x00000010, 0x0000009C,
    0x0000008A, 0x00000025, 0x000500C2, 0x0000000E, 0x0000009D, 0x00000076, 0x00000018, 0x0004007C,
    0x00000010, 0x0000009E, 0x0000009D, 0x00050084, 0x00000010, 0x0000009F, 0x0000009C, 0x0000009E,
    0x00050080, 0x00000010, 0x000000A0, 0x0000009B, 0x0000009F, 0x000500C2, 0x0000000E, 0x000000A1,
    0x00000074, 0x00000021, 0x0004007C, 0x00000010, 0x000000A2, 0x000000A1, 0x00050084, 0x00000010,
    0x000000A3, 0x000000A0, 0x000000A2, 0x000500C3, 0x00000010, 0x000000A4, 0x00000087, 0x00000020,
    0x00050080, 0x00000010, 0x000000A5, 0x000000A4, 0x000000A3, 0x000500C4, 0x00000010, 0x000000A6,
    0x000000A5, 0x00000022, 0x000500C7, 0x00000010, 0x000000A7, 0x000000A6, 0x00000032, 0x000500C4,
    0x00000010, 0x000000A8, 0x000000A7, 0x00000027, 0x000500C7, 0x00000010, 0x000000A9, 0x00000087,
    0x00000023, 0x000500C7, 0x00000010, 0x000000AA, 0x0000008D, 0x0000002F, 0x000500C4, 0x00000010,
    0x000000AB, 0x000000AA, 0x00000025, 0x00050080, 0x00000010, 0x000000AC, 0x000000A9, 0x000000AB,
    0x000500C4, 0x00000010, 0x000000AD, 0x000000AC, 0x00000022, 0x000500C3, 0x00000010, 0x000000AE,
    0x000000AD, 0x0000002F, 0x000500C3, 0x00000010, 0x000000AF, 0x0000008D, 0x0000002B, 0x00050080,
    0x00000010, 0x000000B0, 0x000000AF, 0x0000009C, 0x000500C7, 0x00000010, 0x000000B1, 0x000000B0,
    0x00000027, 0x000500C3, 0x00000010, 0x000000B2, 0x00000087, 0x0000002B, 0x000500C4, 0x00000010,
    0x000000B3, 0x000000B1, 0x00000027, 0x00050080, 0x00000010, 0x000000B4, 0x000000B2, 0x000000B3,
    0x000500C7, 0x00000010, 0x000000B5, 0x000000B4, 0x0000002B, 0x000500C4, 0x00000010, 0x000000B6,
    0x000000B5, 0x00000027, 0x00050080, 0x00000010, 0x000000B7, 0x000000B1, 0x000000B6, 0x000500C7,
    0x00000010, 0x000000B8, 0x000000AE, 0x00000026, 0x00050080, 0x00000010, 0x000000B9, 0x000000A8,
    0x000000B8, 0x000500C4, 0x00000010, 0x000000BA, 0x000000B9, 0x00000027, 0x000500C7, 0x00000010,
    0x000000BB, 0x000000AE, 0x00000028, 0x00050080, 0x00000010, 0x000000BC, 0x000000BA, 0x000000BB,
    0x000500C7, 0x00000010, 0x000000BD, 0x0000008A, 0x0000002B, 0x000500C4, 0x00000010, 0x000000BE,
    0x000000BD, 0x00000022, 0x00050080, 0x00000010, 0x000000BF, 0x000000BC, 0x000000BE, 0x000500C7,
    0x00000010, 0x000000C0, 0x0000008D, 0x00000027, 0x000500C4, 0x00000010, 0x000000C1, 0x000000C0,
    0x00000029, 0x00050080, 0x00000010, 0x000000C2, 0x000000BF, 0x000000C1, 0x000500C7, 0x00000010,
    0x000000C3, 0x000000B7, 0x00000027, 0x000500C4, 0x00000010, 0x000000C4, 0x000000C3, 0x0000002B,
    0x000500C3, 0x00000010, 0x000000C5, 0x000000C2, 0x0000002F, 0x000500C7, 0x00000010, 0x000000C6,
    0x000000C5, 0x00000023, 0x00050080, 0x00000010, 0x000000C7, 0x000000C4, 0x000000C6, 0x000500C4,
    0x00000010, 0x000000C8, 0x000000C7, 0x0000002B, 0x000500C7, 0x00000010, 0x000000C9, 0x000000B7,
    0x00000033, 0x00050080, 0x00000010, 0x000000CA, 0x000000C8, 0x000000C9, 0x000500C4, 0x00000010,
    0x000000CB, 0x000000CA, 0x00000025, 0x000500C7, 0x00000010, 0x000000CC, 0x000000C2, 0x0000002A,
    0x00050080, 0x00000010, 0x000000CD, 0x000000CB, 0x000000CC, 0x000500C4, 0x00000010, 0x000000CE,
    0x000000CD, 0x0000002B, 0x000500C7, 0x00000010, 0x000000CF, 0x000000C2, 0x00000030, 0x00050080,
    0x00000010, 0x000000D0, 0x000000CE, 0x000000CF, 0x000200F9, 0x00000098, 0x000200F8, 0x0000009A,
    0x0004007C, 0x00000011, 0x000000D1, 0x0000007F, 0x00050051, 0x00000010, 0x000000D2, 0x000000D1,
    0x00000000, 0x000500C3, 0x00000010, 0x000000D3, 0x000000D2, 0x00000020, 0x00050051, 0x00000010,
    0x000000D4, 0x000000D1, 0x00000001, 0x000500C3, 0x00000010, 0x000000D5, 0x000000D4, 0x00000020,
    0x000500C2, 0x0000000E, 0x000000D6, 0x00000074, 0x00000021, 0x0004007C, 0x00000010, 0x000000D7,
    0x000000D6, 0x00050084, 0x00000010, 0x000000D8, 0x000000D5, 0x000000D7, 0x00050080, 0x00000010,
    0x000000D9, 0x000000D3, 0x000000D8, 0x000500C4, 0x00000010, 0x000000DA, 0x000000D9, 0x0000001D,
    0x000500C7, 0x00000010, 0x000000DB, 0x000000D2, 0x00000023, 0x000500C7, 0x00000010, 0x000000DC,
    0x000000D4, 0x00000024, 0x000500C4, 0x00000010, 0x000000DD, 0x000000DC, 0x00000025, 0x00050080,
    0x00000010, 0x000000DE, 0x000000DB, 0x000000DD, 0x000500C4, 0x00000010, 0x000000DF, 0x000000DE,
    0x0000001B, 0x000500C7, 0x00000010, 0x000000E0, 0x000000DF, 0x00000026, 0x000500C4, 0x00000010,
    0x000000E1, 0x000000E0, 0x00000027, 0x00050080, 0x00000010, 0x000000E2, 0x000000DA, 0x000000E1,
    0x000500C7, 0x00000010, 0x000000E3, 0x000000DF, 0x00000028, 0x00050080, 0x00000010, 0x000000E4,
    0x000000E2, 0x000000E3, 0x000500C7, 0x00000010, 0x000000E5, 0x000000D4, 0x00000027, 0x000500C4,
    0x00000010, 0x000000E6, 0x000000E5, 0x00000029, 0x00050080, 0x00000010, 0x000000E7, 0x000000E4,
    0x000000E6, 0x000500C7, 0x00000010, 0x000000E8, 0x000000E7, 0x0000002A, 0x000500C4, 0x00000010,
    0x000000E9, 0x000000E8, 0x0000002B, 0x000500C7, 0x00000010, 0x000000EA, 0x000000D4, 0x0000002C,
    0x000500C4, 0x00000010, 0x000000EB, 0x000000EA, 0x00000023, 0x00050080, 0x00000010, 0x000000EC,
    0x000000E9, 0x000000EB, 0x000500C7, 0x00000010, 0x000000ED, 0x000000E7, 0x0000002D, 0x000500C4,
    0x00000010, 0x000000EE, 0x000000ED, 0x00000025, 0x00050080, 0x00000010, 0x000000EF, 0x000000EC,
    0x000000EE, 0x000500C7, 0x00000010, 0x000000F0, 0x000000D4, 0x0000002E, 0x000500C3, 0x00000010,
    0x000000F1, 0x000000F0, 0x00000025, 0x000500C3, 0x00000010, 0x000000F2, 0x000000D2, 0x0000002B,
    0x00050080, 0x00000010, 0x000000F3, 0x000000F1, 0x000000F2, 0x000500C7, 0x00000010, 0x000000F4,
    0x000000F3, 0x0000002B, 0x000500C4, 0x00000010, 0x000000F5, 0x000000F4, 0x0000002F, 0x00050080,
    0x00000010, 0x000000F6, 0x000000EF, 0x000000F5, 0x000500C7, 0x00000010, 0x000000F7, 0x000000E7,
    0x00000030, 0x00050080, 0x00000010, 0x000000F8, 0x000000F6, 0x000000F7, 0x000200F9, 0x00000098,
    0x000200F8, 0x00000098, 0x000700F5, 0x00000010, 0x000000F9, 0x000000D0, 0x00000099, 0x000000F8,
    0x0000009A, 0x000200F9, 0x00000095, 0x000200F8, 0x00000097, 0x0004007C, 0x00000010, 0x000000FA,
    0x00000076, 0x00050084, 0x00000010, 0x000000FB, 0x0000008A, 0x000000FA, 0x00050080, 0x00000010,
    0x000000FC, 0x000000FB, 0x0000008D, 0x0004007C, 0x00000010, 0x000000FD, 0x00000074, 0x00050084,
    0x00000010, 0x000000FE, 0x000000FC, 0x000000FD, 0x00050080, 0x00000010, 0x000000FF, 0x00000088,
    0x000000FE, 0x000200F9, 0x00000095, 0x000200F8, 0x00000095, 0x000700F5, 0x00000010, 0x00000100,
    0x000000F9, 0x00000098, 0x000000FF, 0x00000097, 0x0004007C, 0x00000010, 0x00000101, 0x00000072,
    0x00050080, 0x00000010, 0x00000102, 0x00000101, 0x00000100, 0x0004007C, 0x0000000E, 0x00000103,
    0x00000102, 0x000500C2, 0x0000000E, 0x00000104, 0x00000103, 0x00000018, 0x00060041, 0x00000040,
    0x00000105, 0x00000007, 0x00000039, 0x00000104, 0x0004003D, 0x0000000F, 0x00000106, 0x00000105,
    0x000500AA, 0x00000013, 0x00000107, 0x00000070, 0x0000001B, 0x000300F7, 0x00000108, 0x00000000,
    0x000400FA, 0x00000107, 0x00000109, 0x00000108, 0x000200F8, 0x00000109, 0x000500C7, 0x0000000F,
    0x0000010A, 0x00000106, 0x00000042, 0x000500C4, 0x0000000F, 0x0000010B, 0x0000010A, 0x00000043,
    0x000500C7, 0x0000000F, 0x0000010C, 0x00000106, 0x00000044, 0x000500C2, 0x0000000F, 0x0000010D,
    0x0000010C, 0x00000043, 0x000500C5, 0x0000000F, 0x0000010E, 0x0000010B, 0x0000010D, 0x000200F9,
    0x00000108, 0x000200F8, 0x00000108, 0x000700F5, 0x0000000F, 0x0000010F, 0x00000106, 0x00000095,
    0x0000010E, 0x00000109, 0x000500C7, 0x0000000F, 0x00000110, 0x0000010F, 0x00000050, 0x000500C2,
    0x0000000F, 0x00000111, 0x0000010F, 0x00000051, 0x0009004F, 0x0000000F, 0x00000112, 0x00000110,
    0x00000111, 0x00000000, 0x00000004, 0x00000001, 0x00000005, 0x0009004F, 0x0000000F, 0x00000113,
    0x00000110, 0x00000111, 0x00000002, 0x00000006, 0x00000003, 0x00000007, 0x00050039, 0x0000000F,
    0x00000114, 0x00000057, 0x00000112, 0x00050039, 0x0000000F, 0x00000115, 0x00000057, 0x00000113,
    0x00060041, 0x00000040, 0x00000116, 0x0000000A, 0x00000039, 0x00000094, 0x0003003E, 0x00000116,
    0x00000114, 0x00050080, 0x0000000E, 0x00000117, 0x00000094, 0x00000027, 0x00060041, 0x00000040,
    0x00000118, 0x0000000A, 0x00000039, 0x00000117, 0x0003003E, 0x00000118, 0x00000115, 0x000300F7,
    0x00000119, 0x00000002, 0x000400FA, 0x0000006C, 0x0000011A, 0x0000011B, 0x000200F8, 0x0000011A,
    0x000200F9, 0x00000119, 0x000200F8, 0x0000011B, 0x000200F9, 0x00000119, 0x000200F8, 0x00000119,
    0x000700F5, 0x0000000E, 0x0000011C, 0x00000036, 0x0000011A, 0x00000049, 0x0000011B, 0x000500C2,
    0x0000000E, 0x0000011D, 0x0000011C, 0x00000018, 0x00050080, 0x0000000E, 0x0000011E, 0x00000104,
    0x0000011D, 0x00060041, 0x00000040, 0x0000011F, 0x00000007, 0x00000039, 0x0000011E, 0x0004003D,
    0x0000000F, 0x00000120, 0x0000011F, 0x000300F7, 0x00000121, 0x00000000, 0x000400FA, 0x00000107,
    0x00000122, 0x00000121, 0x000200F8, 0x00000122, 0x000500C7, 0x0000000F, 0x00000123, 0x00000120,
    0x00000042, 0x000500C4, 0x0000000F, 0x00000124, 0x00000123, 0x00000043, 0x000500C7, 0x0000000F,
    0x00000125, 0x00000120, 0x00000044, 0x000500C2, 0x0000000F, 0x00000126, 0x00000125, 0x00000043,
    0x000500C5, 0x0000000F, 0x00000127, 0x00000124, 0x00000126, 0x000200F9, 0x00000121, 0x000200F8,
    0x00000121, 0x000700F5, 0x0000000F, 0x00000128, 0x00000120, 0x00000119, 0x00000127, 0x00000122,
    0x000500C7, 0x0000000F, 0x00000129, 0x00000128, 0x00000050, 0x000500C2, 0x0000000F, 0x0000012A,
    0x00000128, 0x00000051, 0x0009004F, 0x0000000F, 0x0000012B, 0x00000129, 0x0000012A, 0x00000000,
    0x00000004, 0x00000001, 0x00000005, 0x0009004F, 0x0000000F, 0x0000012C, 0x00000129, 0x0000012A,
    0x00000002, 0x00000006, 0x00000003, 0x00000007, 0x00050039, 0x0000000F, 0x0000012D, 0x00000057,
    0x0000012B, 0x00050039, 0x0000000F, 0x0000012E, 0x00000057, 0x0000012C, 0x00050080, 0x0000000E,
    0x0000012F, 0x00000117, 0x00000027, 0x00060041, 0x00000040, 0x00000130, 0x0000000A, 0x00000039,
    0x0000012F, 0x0003003E, 0x00000130, 0x0000012D, 0x00050080, 0x0000000E, 0x00000131, 0x0000012F,
    0x00000027, 0x00060041, 0x00000040, 0x00000132, 0x0000000A, 0x00000039, 0x00000131, 0x0003003E,
    0x00000132, 0x0000012E, 0x000200F9, 0x00000067, 0x000200F8, 0x00000067, 0x000100FD, 0x00010038,
};
