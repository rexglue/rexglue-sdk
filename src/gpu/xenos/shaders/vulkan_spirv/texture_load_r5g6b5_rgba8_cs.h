// Generated with `spirv-as` from the R5G6B5 texture load shader.
#if 0
; SPIR-V
; Version: 1.0
; Generator: Khronos SPIR-V Tools Assembler; 0
; Bound: 323
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
    %uint_31 = OpConstant %uint 31
    %uint_63 = OpConstant %uint 63
    %uint_11 = OpConstant %uint 11
         %90 = OpConstantComposite %v4uint %uint_31 %uint_31 %uint_31 %uint_31
         %91 = OpConstantComposite %v4uint %uint_63 %uint_63 %uint_63 %uint_63
         %92 = OpConstantComposite %v4uint %uint_2 %uint_2 %uint_2 %uint_2
         %93 = OpConstantComposite %v4uint %uint_3 %uint_3 %uint_3 %uint_3
         %94 = OpConstantComposite %v4uint %uint_5 %uint_5 %uint_5 %uint_5
         %95 = OpConstantComposite %v4uint %uint_11 %uint_11 %uint_11 %uint_11
         %96 = OpFunction %v4uint None %22
         %97 = OpFunctionParameter %v4uint
         %98 = OpLabel
         %99 = OpBitwiseAnd %v4uint %97 %90
        %100 = OpShiftLeftLogical %v4uint %99 %93
        %101 = OpShiftRightLogical %v4uint %99 %92
        %102 = OpBitwiseOr %v4uint %100 %101
        %103 = OpShiftRightLogical %v4uint %97 %94
        %104 = OpBitwiseAnd %v4uint %103 %91
        %105 = OpShiftLeftLogical %v4uint %104 %92
        %106 = OpShiftRightLogical %v4uint %104 %70
        %107 = OpBitwiseOr %v4uint %105 %106
        %108 = OpShiftRightLogical %v4uint %97 %95
        %109 = OpShiftLeftLogical %v4uint %108 %93
        %110 = OpShiftRightLogical %v4uint %108 %92
        %111 = OpBitwiseOr %v4uint %109 %110
        %112 = OpShiftLeftLogical %v4uint %107 %67
        %113 = OpShiftLeftLogical %v4uint %111 %81
        %114 = OpShiftLeftLogical %v4uint %111 %83
        %115 = OpBitwiseOr %v4uint %102 %112
        %116 = OpBitwiseOr %v4uint %113 %114
        %117 = OpBitwiseOr %v4uint %115 %116
               OpReturnValue %117
               OpFunctionEnd
          %2 = OpFunction %void None %13
        %118 = OpLabel
               OpSelectionMerge %119 None
               OpSwitch %uint_0 %120
        %120 = OpLabel
        %121 = OpAccessChain %_ptr_PushConstant_uint %56 %int_0
        %122 = OpLoad %uint %121
        %123 = OpBitwiseAnd %uint %122 %uint_1
        %124 = OpINotEqual %bool %123 %uint_0
        %125 = OpBitwiseAnd %uint %122 %uint_2
        %126 = OpINotEqual %bool %125 %uint_0
        %127 = OpShiftRightLogical %uint %122 %uint_2
        %128 = OpBitwiseAnd %uint %127 %uint_3
        %129 = OpAccessChain %_ptr_PushConstant_uint %56 %int_1
        %130 = OpLoad %uint %129
        %131 = OpAccessChain %_ptr_PushConstant_uint %56 %int_2
        %132 = OpLoad %uint %131
        %133 = OpAccessChain %_ptr_PushConstant_uint %56 %int_3
        %134 = OpLoad %uint %133
        %135 = OpAccessChain %_ptr_PushConstant_v3uint %56 %int_4
        %136 = OpLoad %v3uint %135
        %137 = OpAccessChain %_ptr_PushConstant_uint %56 %int_5
        %138 = OpLoad %uint %137
        %139 = OpAccessChain %_ptr_PushConstant_uint %56 %int_6
        %140 = OpLoad %uint %139
        %141 = OpLoad %v3uint %gl_GlobalInvocationID
        %142 = OpShiftLeftLogical %v3uint %141 %61
        %143 = OpVectorShuffle %v2uint %142 %142 0 1
        %144 = OpVectorShuffle %v2uint %136 %136 0 1
        %145 = OpUGreaterThanEqual %v2bool %143 %144
        %146 = OpAny %bool %145
               OpSelectionMerge %147 DontFlatten
               OpBranchConditional %146 %148 %147
        %148 = OpLabel
               OpBranch %119
        %147 = OpLabel
        %149 = OpBitcast %v3int %142
        %150 = OpCompositeExtract %uint %136 1
        %151 = OpCompositeExtract %int %149 0
        %152 = OpIMul %int %151 %int_2
        %153 = OpIMul %int %151 %int_4
        %154 = OpCompositeExtract %int %149 2
        %155 = OpBitcast %int %150
        %156 = OpIMul %int %154 %155
        %157 = OpCompositeExtract %int %149 1
        %158 = OpIAdd %int %156 %157
        %159 = OpBitcast %int %140
        %160 = OpIMul %int %158 %159
        %161 = OpIAdd %int %153 %160
        %162 = OpBitcast %uint %161
        %163 = OpIAdd %uint %162 %138
        %164 = OpShiftRightLogical %uint %163 %uint_4
               OpSelectionMerge %165 DontFlatten
               OpBranchConditional %124 %166 %167
        %166 = OpLabel
               OpSelectionMerge %168 DontFlatten
               OpBranchConditional %126 %169 %170
        %169 = OpLabel
        %171 = OpShiftRightArithmetic %int %157 %int_4
        %172 = OpShiftRightArithmetic %int %154 %int_2
        %173 = OpShiftRightLogical %uint %134 %uint_4
        %174 = OpBitcast %int %173
        %175 = OpIMul %int %172 %174
        %176 = OpIAdd %int %171 %175
        %177 = OpShiftRightLogical %uint %132 %uint_5
        %178 = OpBitcast %int %177
        %179 = OpIMul %int %176 %178
        %180 = OpShiftRightArithmetic %int %151 %int_5
        %181 = OpIAdd %int %180 %179
        %182 = OpShiftLeftLogical %int %181 %uint_7
        %183 = OpBitwiseAnd %int %182 %int_268435455
        %184 = OpShiftLeftLogical %int %183 %int_1
        %185 = OpBitwiseAnd %int %151 %int_7
        %186 = OpBitwiseAnd %int %157 %int_6
        %187 = OpShiftLeftLogical %int %186 %int_2
        %188 = OpIAdd %int %185 %187
        %189 = OpShiftLeftLogical %int %188 %uint_7
        %190 = OpShiftRightArithmetic %int %189 %int_6
        %191 = OpShiftRightArithmetic %int %157 %int_3
        %192 = OpIAdd %int %191 %172
        %193 = OpBitwiseAnd %int %192 %int_1
        %194 = OpShiftRightArithmetic %int %151 %int_3
        %195 = OpShiftLeftLogical %int %193 %int_1
        %196 = OpIAdd %int %194 %195
        %197 = OpBitwiseAnd %int %196 %int_3
        %198 = OpShiftLeftLogical %int %197 %int_1
        %199 = OpIAdd %int %193 %198
        %200 = OpBitwiseAnd %int %190 %int_n16
        %201 = OpIAdd %int %184 %200
        %202 = OpShiftLeftLogical %int %201 %int_1
        %203 = OpBitwiseAnd %int %190 %int_15
        %204 = OpIAdd %int %202 %203
        %205 = OpBitwiseAnd %int %154 %int_3
        %206 = OpShiftLeftLogical %int %205 %uint_7
        %207 = OpIAdd %int %204 %206
        %208 = OpBitwiseAnd %int %157 %int_1
        %209 = OpShiftLeftLogical %int %208 %int_4
        %210 = OpIAdd %int %207 %209
        %211 = OpBitwiseAnd %int %199 %int_1
        %212 = OpShiftLeftLogical %int %211 %int_3
        %213 = OpShiftRightArithmetic %int %210 %int_6
        %214 = OpBitwiseAnd %int %213 %int_7
        %215 = OpIAdd %int %212 %214
        %216 = OpShiftLeftLogical %int %215 %int_3
        %217 = OpBitwiseAnd %int %199 %int_n2
        %218 = OpIAdd %int %216 %217
        %219 = OpShiftLeftLogical %int %218 %int_2
        %220 = OpBitwiseAnd %int %210 %int_n512
        %221 = OpIAdd %int %219 %220
        %222 = OpShiftLeftLogical %int %221 %int_3
        %223 = OpBitwiseAnd %int %210 %int_63
        %224 = OpIAdd %int %222 %223
               OpBranch %168
        %170 = OpLabel
        %225 = OpBitcast %v2int %143
        %226 = OpCompositeExtract %int %225 0
        %227 = OpShiftRightArithmetic %int %226 %int_5
        %228 = OpCompositeExtract %int %225 1
        %229 = OpShiftRightArithmetic %int %228 %int_5
        %230 = OpShiftRightLogical %uint %132 %uint_5
        %231 = OpBitcast %int %230
        %232 = OpIMul %int %229 %231
        %233 = OpIAdd %int %227 %232
        %234 = OpShiftLeftLogical %int %233 %uint_8
        %235 = OpBitwiseAnd %int %226 %int_7
        %236 = OpBitwiseAnd %int %228 %int_14
        %237 = OpShiftLeftLogical %int %236 %int_2
        %238 = OpIAdd %int %235 %237
        %239 = OpShiftLeftLogical %int %238 %uint_1
        %240 = OpBitwiseAnd %int %239 %int_n16
        %241 = OpShiftLeftLogical %int %240 %int_1
        %242 = OpIAdd %int %234 %241
        %243 = OpBitwiseAnd %int %239 %int_15
        %244 = OpIAdd %int %242 %243
        %245 = OpBitwiseAnd %int %228 %int_1
        %246 = OpShiftLeftLogical %int %245 %int_4
        %247 = OpIAdd %int %244 %246
        %248 = OpBitwiseAnd %int %247 %int_n512
        %249 = OpShiftLeftLogical %int %248 %int_3
        %250 = OpBitwiseAnd %int %228 %int_16
        %251 = OpShiftLeftLogical %int %250 %int_7
        %252 = OpIAdd %int %249 %251
        %253 = OpBitwiseAnd %int %247 %int_448
        %254 = OpShiftLeftLogical %int %253 %int_2
        %255 = OpIAdd %int %252 %254
        %256 = OpBitwiseAnd %int %228 %int_8
        %257 = OpShiftRightArithmetic %int %256 %int_2
        %258 = OpShiftRightArithmetic %int %226 %int_3
        %259 = OpIAdd %int %257 %258
        %260 = OpBitwiseAnd %int %259 %int_3
        %261 = OpShiftLeftLogical %int %260 %int_6
        %262 = OpIAdd %int %255 %261
        %263 = OpBitwiseAnd %int %247 %int_63
        %264 = OpIAdd %int %262 %263
               OpBranch %168
        %168 = OpLabel
        %265 = OpPhi %int %224 %169 %264 %170
               OpBranch %165
        %167 = OpLabel
        %266 = OpBitcast %int %134
        %267 = OpIMul %int %154 %266
        %268 = OpIAdd %int %267 %157
        %269 = OpBitcast %int %132
        %270 = OpIMul %int %268 %269
        %271 = OpIAdd %int %152 %270
               OpBranch %165
        %165 = OpLabel
        %272 = OpPhi %int %265 %168 %271 %167
        %273 = OpBitcast %int %130
        %274 = OpIAdd %int %273 %272
        %275 = OpBitcast %uint %274
        %276 = OpShiftRightLogical %uint %275 %uint_4
        %277 = OpAccessChain %_ptr_Uniform_v4uint %7 %int_0 %276
        %278 = OpLoad %v4uint %277
        %279 = OpIEqual %bool %128 %uint_1
               OpSelectionMerge %280 None
               OpBranchConditional %279 %281 %280
        %281 = OpLabel
        %282 = OpBitwiseAnd %v4uint %278 %66
        %283 = OpShiftLeftLogical %v4uint %282 %67
        %284 = OpBitwiseAnd %v4uint %278 %68
        %285 = OpShiftRightLogical %v4uint %284 %67
        %286 = OpBitwiseOr %v4uint %283 %285
               OpBranch %280
        %280 = OpLabel
        %287 = OpPhi %v4uint %278 %165 %286 %281
        %288 = OpBitwiseAnd %v4uint %287 %80
        %289 = OpShiftRightLogical %v4uint %287 %81
        %290 = OpVectorShuffle %v4uint %288 %289 0 4 1 5
        %291 = OpVectorShuffle %v4uint %288 %289 2 6 3 7
        %292 = OpFunctionCall %v4uint %96 %290
        %293 = OpFunctionCall %v4uint %96 %291
        %294 = OpAccessChain %_ptr_Uniform_v4uint %10 %int_0 %164
               OpStore %294 %292
        %295 = OpIAdd %uint %164 %int_1
        %296 = OpAccessChain %_ptr_Uniform_v4uint %10 %int_0 %295
               OpStore %296 %293
               OpSelectionMerge %297 DontFlatten
               OpBranchConditional %124 %298 %299
        %298 = OpLabel
               OpBranch %297
        %299 = OpLabel
               OpBranch %297
        %297 = OpLabel
        %300 = OpPhi %uint %uint_64 %298 %uint_16 %299
        %301 = OpShiftRightLogical %uint %300 %uint_4
        %302 = OpIAdd %uint %276 %301
        %303 = OpAccessChain %_ptr_Uniform_v4uint %7 %int_0 %302
        %304 = OpLoad %v4uint %303
               OpSelectionMerge %305 None
               OpBranchConditional %279 %306 %305
        %306 = OpLabel
        %307 = OpBitwiseAnd %v4uint %304 %66
        %308 = OpShiftLeftLogical %v4uint %307 %67
        %309 = OpBitwiseAnd %v4uint %304 %68
        %310 = OpShiftRightLogical %v4uint %309 %67
        %311 = OpBitwiseOr %v4uint %308 %310
               OpBranch %305
        %305 = OpLabel
        %312 = OpPhi %v4uint %304 %297 %311 %306
        %313 = OpBitwiseAnd %v4uint %312 %80
        %314 = OpShiftRightLogical %v4uint %312 %81
        %315 = OpVectorShuffle %v4uint %313 %314 0 4 1 5
        %316 = OpVectorShuffle %v4uint %313 %314 2 6 3 7
        %317 = OpFunctionCall %v4uint %96 %315
        %318 = OpFunctionCall %v4uint %96 %316
        %319 = OpIAdd %uint %295 %int_1
        %320 = OpAccessChain %_ptr_Uniform_v4uint %10 %int_0 %319
               OpStore %320 %317
        %321 = OpIAdd %uint %319 %int_1
        %322 = OpAccessChain %_ptr_Uniform_v4uint %10 %int_0 %321
               OpStore %322 %318
               OpBranch %119
        %119 = OpLabel
               OpReturn
               OpFunctionEnd
#endif

const uint32_t texture_load_r5g6b5_rgba8_cs[] = {
    0x07230203, 0x00010000, 0x00070000, 0x00000143, 0x00000000, 0x00020011, 0x00000001, 0x0006000B,
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
    0x0004002B, 0x0000000E, 0x00000057, 0x0000001F, 0x0004002B, 0x0000000E, 0x00000058, 0x0000003F,
    0x0004002B, 0x0000000E, 0x00000059, 0x0000000B, 0x0007002C, 0x0000000F, 0x0000005A, 0x00000057,
    0x00000057, 0x00000057, 0x00000057, 0x0007002C, 0x0000000F, 0x0000005B, 0x00000058, 0x00000058,
    0x00000058, 0x00000058, 0x0007002C, 0x0000000F, 0x0000005C, 0x00000031, 0x00000031, 0x00000031,
    0x00000031, 0x0007002C, 0x0000000F, 0x0000005D, 0x00000034, 0x00000034, 0x00000034, 0x00000034,
    0x0007002C, 0x0000000F, 0x0000005E, 0x00000021, 0x00000021, 0x00000021, 0x00000021, 0x0007002C,
    0x0000000F, 0x0000005F, 0x00000059, 0x00000059, 0x00000059, 0x00000059, 0x00050036, 0x0000000F,
    0x00000060, 0x00000000, 0x00000016, 0x00030037, 0x0000000F, 0x00000061, 0x000200F8, 0x00000062,
    0x000500C7, 0x0000000F, 0x00000063, 0x00000061, 0x0000005A, 0x000500C4, 0x0000000F, 0x00000064,
    0x00000063, 0x0000005D, 0x000500C2, 0x0000000F, 0x00000065, 0x00000063, 0x0000005C, 0x000500C5,
    0x0000000F, 0x00000066, 0x00000064, 0x00000065, 0x000500C2, 0x0000000F, 0x00000067, 0x00000061,
    0x0000005E, 0x000500C7, 0x0000000F, 0x00000068, 0x00000067, 0x0000005B, 0x000500C4, 0x0000000F,
    0x00000069, 0x00000068, 0x0000005C, 0x000500C2, 0x0000000F, 0x0000006A, 0x00000068, 0x00000046,
    0x000500C5, 0x0000000F, 0x0000006B, 0x00000069, 0x0000006A, 0x000500C2, 0x0000000F, 0x0000006C,
    0x00000061, 0x0000005F, 0x000500C4, 0x0000000F, 0x0000006D, 0x0000006C, 0x0000005D, 0x000500C2,
    0x0000000F, 0x0000006E, 0x0000006C, 0x0000005C, 0x000500C5, 0x0000000F, 0x0000006F, 0x0000006D,
    0x0000006E, 0x000500C4, 0x0000000F, 0x00000070, 0x0000006B, 0x00000043, 0x000500C4, 0x0000000F,
    0x00000071, 0x0000006F, 0x00000051, 0x000500C4, 0x0000000F, 0x00000072, 0x0000006F, 0x00000053,
    0x000500C5, 0x0000000F, 0x00000073, 0x00000066, 0x00000070, 0x000500C5, 0x0000000F, 0x00000074,
    0x00000071, 0x00000072, 0x000500C5, 0x0000000F, 0x00000075, 0x00000073, 0x00000074, 0x000200FE,
    0x00000075, 0x00010038, 0x00050036, 0x0000000C, 0x00000002, 0x00000000, 0x0000000D, 0x000200F8,
    0x00000076, 0x000300F7, 0x00000077, 0x00000000, 0x000300FB, 0x0000001F, 0x00000078, 0x000200F8,
    0x00000078, 0x00050041, 0x0000003A, 0x00000079, 0x00000038, 0x00000039, 0x0004003D, 0x0000000E,
    0x0000007A, 0x00000079, 0x000500C7, 0x0000000E, 0x0000007B, 0x0000007A, 0x0000001B, 0x000500AB,
    0x00000013, 0x0000007C, 0x0000007B, 0x0000001F, 0x000500C7, 0x0000000E, 0x0000007D, 0x0000007A,
    0x00000031, 0x000500AB, 0x00000013, 0x0000007E, 0x0000007D, 0x0000001F, 0x000500C2, 0x0000000E,
    0x0000007F, 0x0000007A, 0x00000031, 0x000500C7, 0x0000000E, 0x00000080, 0x0000007F, 0x00000034,
    0x00050041, 0x0000003A, 0x00000081, 0x00000038, 0x00000027, 0x0004003D, 0x0000000E, 0x00000082,
    0x00000081, 0x00050041, 0x0000003A, 0x00000083, 0x00000038, 0x00000025, 0x0004003D, 0x0000000E,
    0x00000084, 0x00000083, 0x00050041, 0x0000003A, 0x00000085, 0x00000038, 0x0000002B, 0x0004003D,
    0x0000000E, 0x00000086, 0x00000085, 0x00050041, 0x0000003B, 0x00000087, 0x00000038, 0x00000029,
    0x0004003D, 0x00000015, 0x00000088, 0x00000087, 0x00050041, 0x0000003A, 0x00000089, 0x00000038,
    0x00000020, 0x0004003D, 0x0000000E, 0x0000008A, 0x00000089, 0x00050041, 0x0000003A, 0x0000008B,
    0x00000038, 0x0000002F, 0x0004003D, 0x0000000E, 0x0000008C, 0x0000008B, 0x0004003D, 0x00000015,
    0x0000008D, 0x00000003, 0x000500C4, 0x00000015, 0x0000008E, 0x0000008D, 0x0000003D, 0x0007004F,
    0x00000014, 0x0000008F, 0x0000008E, 0x0000008E, 0x00000000, 0x00000001, 0x0007004F, 0x00000014,
    0x00000090, 0x00000088, 0x00000088, 0x00000000, 0x00000001, 0x000500AE, 0x0000003E, 0x00000091,
    0x0000008F, 0x00000090, 0x0004009A, 0x00000013, 0x00000092, 0x00000091, 0x000300F7, 0x00000093,
    0x00000002, 0x000400FA, 0x00000092, 0x00000094, 0x00000093, 0x000200F8, 0x00000094, 0x000200F9,
    0x00000077, 0x000200F8, 0x00000093, 0x0004007C, 0x00000012, 0x00000095, 0x0000008E, 0x00050051,
    0x0000000E, 0x00000096, 0x00000088, 0x00000001, 0x00050051, 0x00000010, 0x00000097, 0x00000095,
    0x00000000, 0x00050084, 0x00000010, 0x00000098, 0x00000097, 0x00000025, 0x00050084, 0x00000010,
    0x00000099, 0x00000097, 0x00000029, 0x00050051, 0x00000010, 0x0000009A, 0x00000095, 0x00000002,
    0x0004007C, 0x00000010, 0x0000009B, 0x00000096, 0x00050084, 0x00000010, 0x0000009C, 0x0000009A,
    0x0000009B, 0x00050051, 0x00000010, 0x0000009D, 0x00000095, 0x00000001, 0x00050080, 0x00000010,
    0x0000009E, 0x0000009C, 0x0000009D, 0x0004007C, 0x00000010, 0x0000009F, 0x0000008C, 0x00050084,
    0x00000010, 0x000000A0, 0x0000009E, 0x0000009F, 0x00050080, 0x00000010, 0x000000A1, 0x00000099,
    0x000000A0, 0x0004007C, 0x0000000E, 0x000000A2, 0x000000A1, 0x00050080, 0x0000000E, 0x000000A3,
    0x000000A2, 0x0000008A, 0x000500C2, 0x0000000E, 0x000000A4, 0x000000A3, 0x00000018, 0x000300F7,
    0x000000A5, 0x00000002, 0x000400FA, 0x0000007C, 0x000000A6, 0x000000A7, 0x000200F8, 0x000000A6,
    0x000300F7, 0x000000A8, 0x00000002, 0x000400FA, 0x0000007E, 0x000000A9, 0x000000AA, 0x000200F8,
    0x000000A9, 0x000500C3, 0x00000010, 0x000000AB, 0x0000009D, 0x00000029, 0x000500C3, 0x00000010,
    0x000000AC, 0x0000009A, 0x00000025, 0x000500C2, 0x0000000E, 0x000000AD, 0x00000086, 0x00000018,
    0x0004007C, 0x00000010, 0x000000AE, 0x000000AD, 0x00050084, 0x00000010, 0x000000AF, 0x000000AC,
    0x000000AE, 0x00050080, 0x00000010, 0x000000B0, 0x000000AB, 0x000000AF, 0x000500C2, 0x0000000E,
    0x000000B1, 0x00000084, 0x00000021, 0x0004007C, 0x00000010, 0x000000B2, 0x000000B1, 0x00050084,
    0x00000010, 0x000000B3, 0x000000B0, 0x000000B2, 0x000500C3, 0x00000010, 0x000000B4, 0x00000097,
    0x00000020, 0x00050080, 0x00000010, 0x000000B5, 0x000000B4, 0x000000B3, 0x000500C4, 0x00000010,
    0x000000B6, 0x000000B5, 0x00000022, 0x000500C7, 0x00000010, 0x000000B7, 0x000000B6, 0x00000032,
    0x000500C4, 0x00000010, 0x000000B8, 0x000000B7, 0x00000027, 0x000500C7, 0x00000010, 0x000000B9,
    0x00000097, 0x00000023, 0x000500C7, 0x00000010, 0x000000BA, 0x0000009D, 0x0000002F, 0x000500C4,
    0x00000010, 0x000000BB, 0x000000BA, 0x00000025, 0x00050080, 0x00000010, 0x000000BC, 0x000000B9,
    0x000000BB, 0x000500C4, 0x00000010, 0x000000BD, 0x000000BC, 0x00000022, 0x000500C3, 0x00000010,
    0x000000BE, 0x000000BD, 0x0000002F, 0x000500C3, 0x00000010, 0x000000BF, 0x0000009D, 0x0000002B,
    0x00050080, 0x00000010, 0x000000C0, 0x000000BF, 0x000000AC, 0x000500C7, 0x00000010, 0x000000C1,
    0x000000C0, 0x00000027, 0x000500C3, 0x00000010, 0x000000C2, 0x00000097, 0x0000002B, 0x000500C4,
    0x00000010, 0x000000C3, 0x000000C1, 0x00000027, 0x00050080, 0x00000010, 0x000000C4, 0x000000C2,
    0x000000C3, 0x000500C7, 0x00000010, 0x000000C5, 0x000000C4, 0x0000002B, 0x000500C4, 0x00000010,
    0x000000C6, 0x000000C5, 0x00000027, 0x00050080, 0x00000010, 0x000000C7, 0x000000C1, 0x000000C6,
    0x000500C7, 0x00000010, 0x000000C8, 0x000000BE, 0x00000026, 0x00050080, 0x00000010, 0x000000C9,
    0x000000B8, 0x000000C8, 0x000500C4, 0x00000010, 0x000000CA, 0x000000C9, 0x00000027, 0x000500C7,
    0x00000010, 0x000000CB, 0x000000BE, 0x00000028, 0x00050080, 0x00000010, 0x000000CC, 0x000000CA,
    0x000000CB, 0x000500C7, 0x00000010, 0x000000CD, 0x0000009A, 0x0000002B, 0x000500C4, 0x00000010,
    0x000000CE, 0x000000CD, 0x00000022, 0x00050080, 0x00000010, 0x000000CF, 0x000000CC, 0x000000CE,
    0x000500C7, 0x00000010, 0x000000D0, 0x0000009D, 0x00000027, 0x000500C4, 0x00000010, 0x000000D1,
    0x000000D0, 0x00000029, 0x00050080, 0x00000010, 0x000000D2, 0x000000CF, 0x000000D1, 0x000500C7,
    0x00000010, 0x000000D3, 0x000000C7, 0x00000027, 0x000500C4, 0x00000010, 0x000000D4, 0x000000D3,
    0x0000002B, 0x000500C3, 0x00000010, 0x000000D5, 0x000000D2, 0x0000002F, 0x000500C7, 0x00000010,
    0x000000D6, 0x000000D5, 0x00000023, 0x00050080, 0x00000010, 0x000000D7, 0x000000D4, 0x000000D6,
    0x000500C4, 0x00000010, 0x000000D8, 0x000000D7, 0x0000002B, 0x000500C7, 0x00000010, 0x000000D9,
    0x000000C7, 0x00000033, 0x00050080, 0x00000010, 0x000000DA, 0x000000D8, 0x000000D9, 0x000500C4,
    0x00000010, 0x000000DB, 0x000000DA, 0x00000025, 0x000500C7, 0x00000010, 0x000000DC, 0x000000D2,
    0x0000002A, 0x00050080, 0x00000010, 0x000000DD, 0x000000DB, 0x000000DC, 0x000500C4, 0x00000010,
    0x000000DE, 0x000000DD, 0x0000002B, 0x000500C7, 0x00000010, 0x000000DF, 0x000000D2, 0x00000030,
    0x00050080, 0x00000010, 0x000000E0, 0x000000DE, 0x000000DF, 0x000200F9, 0x000000A8, 0x000200F8,
    0x000000AA, 0x0004007C, 0x00000011, 0x000000E1, 0x0000008F, 0x00050051, 0x00000010, 0x000000E2,
    0x000000E1, 0x00000000, 0x000500C3, 0x00000010, 0x000000E3, 0x000000E2, 0x00000020, 0x00050051,
    0x00000010, 0x000000E4, 0x000000E1, 0x00000001, 0x000500C3, 0x00000010, 0x000000E5, 0x000000E4,
    0x00000020, 0x000500C2, 0x0000000E, 0x000000E6, 0x00000084, 0x00000021, 0x0004007C, 0x00000010,
    0x000000E7, 0x000000E6, 0x00050084, 0x00000010, 0x000000E8, 0x000000E5, 0x000000E7, 0x00050080,
    0x00000010, 0x000000E9, 0x000000E3, 0x000000E8, 0x000500C4, 0x00000010, 0x000000EA, 0x000000E9,
    0x0000001D, 0x000500C7, 0x00000010, 0x000000EB, 0x000000E2, 0x00000023, 0x000500C7, 0x00000010,
    0x000000EC, 0x000000E4, 0x00000024, 0x000500C4, 0x00000010, 0x000000ED, 0x000000EC, 0x00000025,
    0x00050080, 0x00000010, 0x000000EE, 0x000000EB, 0x000000ED, 0x000500C4, 0x00000010, 0x000000EF,
    0x000000EE, 0x0000001B, 0x000500C7, 0x00000010, 0x000000F0, 0x000000EF, 0x00000026, 0x000500C4,
    0x00000010, 0x000000F1, 0x000000F0, 0x00000027, 0x00050080, 0x00000010, 0x000000F2, 0x000000EA,
    0x000000F1, 0x000500C7, 0x00000010, 0x000000F3, 0x000000EF, 0x00000028, 0x00050080, 0x00000010,
    0x000000F4, 0x000000F2, 0x000000F3, 0x000500C7, 0x00000010, 0x000000F5, 0x000000E4, 0x00000027,
    0x000500C4, 0x00000010, 0x000000F6, 0x000000F5, 0x00000029, 0x00050080, 0x00000010, 0x000000F7,
    0x000000F4, 0x000000F6, 0x000500C7, 0x00000010, 0x000000F8, 0x000000F7, 0x0000002A, 0x000500C4,
    0x00000010, 0x000000F9, 0x000000F8, 0x0000002B, 0x000500C7, 0x00000010, 0x000000FA, 0x000000E4,
    0x0000002C, 0x000500C4, 0x00000010, 0x000000FB, 0x000000FA, 0x00000023, 0x00050080, 0x00000010,
    0x000000FC, 0x000000F9, 0x000000FB, 0x000500C7, 0x00000010, 0x000000FD, 0x000000F7, 0x0000002D,
    0x000500C4, 0x00000010, 0x000000FE, 0x000000FD, 0x00000025, 0x00050080, 0x00000010, 0x000000FF,
    0x000000FC, 0x000000FE, 0x000500C7, 0x00000010, 0x00000100, 0x000000E4, 0x0000002E, 0x000500C3,
    0x00000010, 0x00000101, 0x00000100, 0x00000025, 0x000500C3, 0x00000010, 0x00000102, 0x000000E2,
    0x0000002B, 0x00050080, 0x00000010, 0x00000103, 0x00000101, 0x00000102, 0x000500C7, 0x00000010,
    0x00000104, 0x00000103, 0x0000002B, 0x000500C4, 0x00000010, 0x00000105, 0x00000104, 0x0000002F,
    0x00050080, 0x00000010, 0x00000106, 0x000000FF, 0x00000105, 0x000500C7, 0x00000010, 0x00000107,
    0x000000F7, 0x00000030, 0x00050080, 0x00000010, 0x00000108, 0x00000106, 0x00000107, 0x000200F9,
    0x000000A8, 0x000200F8, 0x000000A8, 0x000700F5, 0x00000010, 0x00000109, 0x000000E0, 0x000000A9,
    0x00000108, 0x000000AA, 0x000200F9, 0x000000A5, 0x000200F8, 0x000000A7, 0x0004007C, 0x00000010,
    0x0000010A, 0x00000086, 0x00050084, 0x00000010, 0x0000010B, 0x0000009A, 0x0000010A, 0x00050080,
    0x00000010, 0x0000010C, 0x0000010B, 0x0000009D, 0x0004007C, 0x00000010, 0x0000010D, 0x00000084,
    0x00050084, 0x00000010, 0x0000010E, 0x0000010C, 0x0000010D, 0x00050080, 0x00000010, 0x0000010F,
    0x00000098, 0x0000010E, 0x000200F9, 0x000000A5, 0x000200F8, 0x000000A5, 0x000700F5, 0x00000010,
    0x00000110, 0x00000109, 0x000000A8, 0x0000010F, 0x000000A7, 0x0004007C, 0x00000010, 0x00000111,
    0x00000082, 0x00050080, 0x00000010, 0x00000112, 0x00000111, 0x00000110, 0x0004007C, 0x0000000E,
    0x00000113, 0x00000112, 0x000500C2, 0x0000000E, 0x00000114, 0x00000113, 0x00000018, 0x00060041,
    0x00000040, 0x00000115, 0x00000007, 0x00000039, 0x00000114, 0x0004003D, 0x0000000F, 0x00000116,
    0x00000115, 0x000500AA, 0x00000013, 0x00000117, 0x00000080, 0x0000001B, 0x000300F7, 0x00000118,
    0x00000000, 0x000400FA, 0x00000117, 0x00000119, 0x00000118, 0x000200F8, 0x00000119, 0x000500C7,
    0x0000000F, 0x0000011A, 0x00000116, 0x00000042, 0x000500C4, 0x0000000F, 0x0000011B, 0x0000011A,
    0x00000043, 0x000500C7, 0x0000000F, 0x0000011C, 0x00000116, 0x00000044, 0x000500C2, 0x0000000F,
    0x0000011D, 0x0000011C, 0x00000043, 0x000500C5, 0x0000000F, 0x0000011E, 0x0000011B, 0x0000011D,
    0x000200F9, 0x00000118, 0x000200F8, 0x00000118, 0x000700F5, 0x0000000F, 0x0000011F, 0x00000116,
    0x000000A5, 0x0000011E, 0x00000119, 0x000500C7, 0x0000000F, 0x00000120, 0x0000011F, 0x00000050,
    0x000500C2, 0x0000000F, 0x00000121, 0x0000011F, 0x00000051, 0x0009004F, 0x0000000F, 0x00000122,
    0x00000120, 0x00000121, 0x00000000, 0x00000004, 0x00000001, 0x00000005, 0x0009004F, 0x0000000F,
    0x00000123, 0x00000120, 0x00000121, 0x00000002, 0x00000006, 0x00000003, 0x00000007, 0x00050039,
    0x0000000F, 0x00000124, 0x00000060, 0x00000122, 0x00050039, 0x0000000F, 0x00000125, 0x00000060,
    0x00000123, 0x00060041, 0x00000040, 0x00000126, 0x0000000A, 0x00000039, 0x000000A4, 0x0003003E,
    0x00000126, 0x00000124, 0x00050080, 0x0000000E, 0x00000127, 0x000000A4, 0x00000027, 0x00060041,
    0x00000040, 0x00000128, 0x0000000A, 0x00000039, 0x00000127, 0x0003003E, 0x00000128, 0x00000125,
    0x000300F7, 0x00000129, 0x00000002, 0x000400FA, 0x0000007C, 0x0000012A, 0x0000012B, 0x000200F8,
    0x0000012A, 0x000200F9, 0x00000129, 0x000200F8, 0x0000012B, 0x000200F9, 0x00000129, 0x000200F8,
    0x00000129, 0x000700F5, 0x0000000E, 0x0000012C, 0x00000036, 0x0000012A, 0x00000049, 0x0000012B,
    0x000500C2, 0x0000000E, 0x0000012D, 0x0000012C, 0x00000018, 0x00050080, 0x0000000E, 0x0000012E,
    0x00000114, 0x0000012D, 0x00060041, 0x00000040, 0x0000012F, 0x00000007, 0x00000039, 0x0000012E,
    0x0004003D, 0x0000000F, 0x00000130, 0x0000012F, 0x000300F7, 0x00000131, 0x00000000, 0x000400FA,
    0x00000117, 0x00000132, 0x00000131, 0x000200F8, 0x00000132, 0x000500C7, 0x0000000F, 0x00000133,
    0x00000130, 0x00000042, 0x000500C4, 0x0000000F, 0x00000134, 0x00000133, 0x00000043, 0x000500C7,
    0x0000000F, 0x00000135, 0x00000130, 0x00000044, 0x000500C2, 0x0000000F, 0x00000136, 0x00000135,
    0x00000043, 0x000500C5, 0x0000000F, 0x00000137, 0x00000134, 0x00000136, 0x000200F9, 0x00000131,
    0x000200F8, 0x00000131, 0x000700F5, 0x0000000F, 0x00000138, 0x00000130, 0x00000129, 0x00000137,
    0x00000132, 0x000500C7, 0x0000000F, 0x00000139, 0x00000138, 0x00000050, 0x000500C2, 0x0000000F,
    0x0000013A, 0x00000138, 0x00000051, 0x0009004F, 0x0000000F, 0x0000013B, 0x00000139, 0x0000013A,
    0x00000000, 0x00000004, 0x00000001, 0x00000005, 0x0009004F, 0x0000000F, 0x0000013C, 0x00000139,
    0x0000013A, 0x00000002, 0x00000006, 0x00000003, 0x00000007, 0x00050039, 0x0000000F, 0x0000013D,
    0x00000060, 0x0000013B, 0x00050039, 0x0000000F, 0x0000013E, 0x00000060, 0x0000013C, 0x00050080,
    0x0000000E, 0x0000013F, 0x00000127, 0x00000027, 0x00060041, 0x00000040, 0x00000140, 0x0000000A,
    0x00000039, 0x0000013F, 0x0003003E, 0x00000140, 0x0000013D, 0x00050080, 0x0000000E, 0x00000141,
    0x0000013F, 0x00000027, 0x00060041, 0x00000040, 0x00000142, 0x0000000A, 0x00000039, 0x00000141,
    0x0003003E, 0x00000142, 0x0000013E, 0x000200F9, 0x00000077, 0x000200F8, 0x00000077, 0x000100FD,
    0x00010038,
};
