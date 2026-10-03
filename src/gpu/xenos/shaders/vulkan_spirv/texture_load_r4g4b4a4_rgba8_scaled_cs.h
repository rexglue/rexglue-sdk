// Derived from texture_load_r5g6b5_rgba8_scaled with the R4G4B4A4 transform.
#if 0
; SPIR-V
; Version: 1.0
; Generator: Khronos SPIR-V Tools Assembler; 0
; Bound: 367
; Schema: 0
               OpCapability Shader
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %gl_GlobalInvocationID
               OpExecutionMode %main LocalSize 4 32 1
               OpSource GLSL 460
               OpSourceExtension "GL_EXT_control_flow_attributes"
               OpSourceExtension "GL_EXT_samplerless_texture_functions"
               OpSourceExtension "GL_GOOGLE_cpp_style_line_directive"
               OpSourceExtension "GL_GOOGLE_include_directive"
               OpName %main "main"
               OpName %push_const_block_xe "push_const_block_xe"
               OpMemberName %push_const_block_xe 0 "xe_texture_load_is_tiled_3d_endian_scale"
               OpMemberName %push_const_block_xe 1 "xe_texture_load_guest_offset"
               OpMemberName %push_const_block_xe 2 "xe_texture_load_guest_pitch_aligned"
               OpMemberName %push_const_block_xe 3 "xe_texture_load_guest_z_stride_block_rows_aligned"
               OpMemberName %push_const_block_xe 4 "xe_texture_load_size_blocks"
               OpMemberName %push_const_block_xe 5 "xe_texture_load_host_offset"
               OpMemberName %push_const_block_xe 6 "xe_texture_load_host_pitch"
               OpMemberName %push_const_block_xe 7 "xe_texture_load_height_texels"
               OpName %push_consts_xe "push_consts_xe"
               OpName %gl_GlobalInvocationID "gl_GlobalInvocationID"
               OpName %xe_texture_load_source_xe_block "xe_texture_load_source_xe_block"
               OpMemberName %xe_texture_load_source_xe_block 0 "data"
               OpName %xe_texture_load_source "xe_texture_load_source"
               OpName %xe_texture_load_dest_xe_block "xe_texture_load_dest_xe_block"
               OpMemberName %xe_texture_load_dest_xe_block 0 "data"
               OpName %xe_texture_load_dest "xe_texture_load_dest"
               OpDecorate %push_const_block_xe Block
               OpMemberDecorate %push_const_block_xe 0 Offset 0
               OpMemberDecorate %push_const_block_xe 1 Offset 4
               OpMemberDecorate %push_const_block_xe 2 Offset 8
               OpMemberDecorate %push_const_block_xe 3 Offset 12
               OpMemberDecorate %push_const_block_xe 4 Offset 16
               OpMemberDecorate %push_const_block_xe 5 Offset 28
               OpMemberDecorate %push_const_block_xe 6 Offset 32
               OpMemberDecorate %push_const_block_xe 7 Offset 36
               OpDecorate %gl_GlobalInvocationID BuiltIn GlobalInvocationId
               OpDecorate %_runtimearr_v4uint ArrayStride 16
               OpDecorate %xe_texture_load_source_xe_block BufferBlock
               OpMemberDecorate %xe_texture_load_source_xe_block 0 NonWritable
               OpMemberDecorate %xe_texture_load_source_xe_block 0 Offset 0
               OpDecorate %xe_texture_load_source NonWritable
               OpDecorate %xe_texture_load_source Binding 0
               OpDecorate %xe_texture_load_source DescriptorSet 1
               OpDecorate %_runtimearr_v4uint_0 ArrayStride 16
               OpDecorate %xe_texture_load_dest_xe_block BufferBlock
               OpMemberDecorate %xe_texture_load_dest_xe_block 0 NonReadable
               OpMemberDecorate %xe_texture_load_dest_xe_block 0 Offset 0
               OpDecorate %xe_texture_load_dest NonReadable
               OpDecorate %xe_texture_load_dest Binding 0
               OpDecorate %xe_texture_load_dest DescriptorSet 0
               OpDecorate %gl_WorkGroupSize BuiltIn WorkgroupSize
       %void = OpTypeVoid
         %14 = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %v2uint = OpTypeVector %uint 2
     %v4uint = OpTypeVector %uint 4
        %int = OpTypeInt 32 1
      %v2int = OpTypeVector %int 2
      %v3int = OpTypeVector %int 3
       %bool = OpTypeBool
     %v3uint = OpTypeVector %uint 3
     %uint_1 = OpConstant %uint 1
%uint_16711935 = OpConstant %uint 16711935
     %uint_8 = OpConstant %uint 8
%uint_4278255360 = OpConstant %uint 4278255360
      %int_4 = OpConstant %int 4
      %int_6 = OpConstant %int 6
     %int_11 = OpConstant %int 11
     %int_15 = OpConstant %int 15
      %int_1 = OpConstant %int 1
      %int_5 = OpConstant %int 5
      %int_7 = OpConstant %int 7
      %int_8 = OpConstant %int 8
     %int_12 = OpConstant %int 12
     %uint_0 = OpConstant %uint 0
      %int_3 = OpConstant %int 3
     %uint_2 = OpConstant %uint 2
      %int_2 = OpConstant %int 2
     %uint_3 = OpConstant %uint 3
     %uint_5 = OpConstant %uint 5
     %uint_4 = OpConstant %uint 4
      %int_0 = OpConstant %int 0
%push_const_block_xe = OpTypeStruct %uint %uint %uint %uint %v3uint %uint %uint %uint
%_ptr_PushConstant_push_const_block_xe = OpTypePointer PushConstant %push_const_block_xe
%push_consts_xe = OpVariable %_ptr_PushConstant_push_const_block_xe PushConstant
%_ptr_PushConstant_uint = OpTypePointer PushConstant %uint
     %uint_7 = OpConstant %uint 7
         %47 = OpConstantComposite %v2uint %uint_4 %uint_7
%_ptr_PushConstant_v3uint = OpTypePointer PushConstant %v3uint
    %uint_31 = OpConstant %uint 31
    %uint_63 = OpConstant %uint 63
    %uint_11 = OpConstant %uint 11
    %uint_16 = OpConstant %uint 16
    %uint_24 = OpConstant %uint 24
%_ptr_Input_v3uint = OpTypePointer Input %v3uint
%gl_GlobalInvocationID = OpVariable %_ptr_Input_v3uint Input
         %55 = OpConstantComposite %v3uint %uint_4 %uint_0 %uint_0
     %v2bool = OpTypeVector %bool 2
%_runtimearr_v4uint = OpTypeRuntimeArray %v4uint
%xe_texture_load_source_xe_block = OpTypeStruct %_runtimearr_v4uint
%_ptr_Uniform_xe_texture_load_source_xe_block = OpTypePointer Uniform %xe_texture_load_source_xe_block
%xe_texture_load_source = OpVariable %_ptr_Uniform_xe_texture_load_source_xe_block Uniform
%_ptr_Uniform_v4uint = OpTypePointer Uniform %v4uint
 %uint_65535 = OpConstant %uint 65535
%_runtimearr_v4uint_0 = OpTypeRuntimeArray %v4uint
%xe_texture_load_dest_xe_block = OpTypeStruct %_runtimearr_v4uint_0
%_ptr_Uniform_xe_texture_load_dest_xe_block = OpTypePointer Uniform %xe_texture_load_dest_xe_block
%xe_texture_load_dest = OpVariable %_ptr_Uniform_xe_texture_load_dest_xe_block Uniform
    %uint_32 = OpConstant %uint 32
%gl_WorkGroupSize = OpConstantComposite %v3uint %uint_4 %uint_32 %uint_1
         %62 = OpConstantComposite %v2uint %uint_7 %uint_7
         %63 = OpConstantComposite %v2uint %uint_4 %uint_2
    %uint_15 = OpConstant %uint 15
         %65 = OpConstantComposite %v2uint %uint_15 %uint_3
         %66 = OpConstantComposite %v4uint %uint_16711935 %uint_16711935 %uint_16711935 %uint_16711935
         %67 = OpConstantComposite %v4uint %uint_8 %uint_8 %uint_8 %uint_8
         %68 = OpConstantComposite %v4uint %uint_4278255360 %uint_4278255360 %uint_4278255360 %uint_4278255360
         %69 = OpConstantComposite %v4uint %uint_65535 %uint_65535 %uint_65535 %uint_65535
         %70 = OpConstantComposite %v4uint %uint_16 %uint_16 %uint_16 %uint_16
         %71 = OpConstantComposite %v4uint %uint_31 %uint_31 %uint_31 %uint_31
         %72 = OpConstantComposite %v4uint %uint_3 %uint_3 %uint_3 %uint_3
         %73 = OpConstantComposite %v4uint %uint_2 %uint_2 %uint_2 %uint_2
         %74 = OpConstantComposite %v4uint %uint_5 %uint_5 %uint_5 %uint_5
         %75 = OpConstantComposite %v4uint %uint_63 %uint_63 %uint_63 %uint_63
         %76 = OpConstantComposite %v4uint %uint_4 %uint_4 %uint_4 %uint_4
         %77 = OpConstantComposite %v4uint %uint_11 %uint_11 %uint_11 %uint_11
         %78 = OpConstantComposite %v4uint %uint_24 %uint_24 %uint_24 %uint_24
   %uint_240 = OpConstant %uint 240
  %uint_3840 = OpConstant %uint 3840
 %uint_61440 = OpConstant %uint 61440
    %uint_12 = OpConstant %uint 12
         %83 = OpConstantComposite %v4uint %uint_15 %uint_15 %uint_15 %uint_15
         %84 = OpConstantComposite %v4uint %uint_240 %uint_240 %uint_240 %uint_240
         %85 = OpConstantComposite %v4uint %uint_3840 %uint_3840 %uint_3840 %uint_3840
         %86 = OpConstantComposite %v4uint %uint_61440 %uint_61440 %uint_61440 %uint_61440
         %87 = OpConstantComposite %v4uint %uint_12 %uint_12 %uint_12 %uint_12
       %main = OpFunction %void None %14
         %88 = OpLabel
               OpSelectionMerge %89 None
               OpSwitch %uint_0 %90
         %90 = OpLabel
         %91 = OpAccessChain %_ptr_PushConstant_uint %push_consts_xe %int_0
         %92 = OpLoad %uint %91
         %93 = OpBitwiseAnd %uint %92 %uint_2
         %94 = OpINotEqual %bool %93 %uint_0
         %95 = OpShiftRightLogical %uint %92 %uint_2
         %96 = OpBitwiseAnd %uint %95 %uint_3
         %97 = OpCompositeConstruct %v2uint %92 %92
         %98 = OpShiftRightLogical %v2uint %97 %47
         %99 = OpBitwiseAnd %v2uint %98 %62
        %100 = OpAccessChain %_ptr_PushConstant_uint %push_consts_xe %int_1
        %101 = OpLoad %uint %100
        %102 = OpAccessChain %_ptr_PushConstant_uint %push_consts_xe %int_2
        %103 = OpLoad %uint %102
        %104 = OpAccessChain %_ptr_PushConstant_uint %push_consts_xe %int_3
        %105 = OpLoad %uint %104
        %106 = OpAccessChain %_ptr_PushConstant_v3uint %push_consts_xe %int_4
        %107 = OpLoad %v3uint %106
        %108 = OpAccessChain %_ptr_PushConstant_uint %push_consts_xe %int_5
        %109 = OpLoad %uint %108
        %110 = OpAccessChain %_ptr_PushConstant_uint %push_consts_xe %int_6
        %111 = OpLoad %uint %110
        %112 = OpLoad %v3uint %gl_GlobalInvocationID
        %113 = OpShiftLeftLogical %v3uint %112 %55
        %114 = OpVectorShuffle %v2uint %113 %113 0 1
        %115 = OpVectorShuffle %v2uint %107 %107 0 1
        %116 = OpUGreaterThanEqual %v2bool %114 %115
        %117 = OpAny %bool %116
               OpSelectionMerge %118 DontFlatten
               OpBranchConditional %117 %119 %118
        %119 = OpLabel
               OpBranch %89
        %118 = OpLabel
        %120 = OpBitcast %v3int %113
        %121 = OpCompositeExtract %uint %107 1
        %122 = OpCompositeExtract %int %120 0
        %123 = OpIMul %int %122 %int_4
        %124 = OpCompositeExtract %int %120 2
        %125 = OpBitcast %int %121
        %126 = OpIMul %int %124 %125
        %127 = OpCompositeExtract %int %120 1
        %128 = OpIAdd %int %126 %127
        %129 = OpBitcast %int %111
        %130 = OpIMul %int %128 %129
        %131 = OpIAdd %int %123 %130
        %132 = OpBitcast %uint %131
        %133 = OpIAdd %uint %132 %109
        %134 = OpShiftRightLogical %uint %133 %uint_4
        %135 = OpShiftRightLogical %v2uint %114 %63
        %136 = OpUDiv %v2uint %135 %99
        %137 = OpIMul %v2uint %99 %136
        %138 = OpISub %v2uint %135 %137
        %139 = OpShiftLeftLogical %v2uint %136 %63
        %140 = OpCompositeExtract %uint %138 0
        %141 = OpCompositeExtract %uint %99 1
        %142 = OpIMul %uint %140 %141
        %143 = OpCompositeExtract %uint %138 1
        %144 = OpIAdd %uint %142 %143
        %145 = OpBitwiseAnd %v2uint %114 %65
        %146 = OpShiftLeftLogical %uint %144 %uint_7
        %147 = OpCompositeExtract %uint %145 1
        %148 = OpShiftLeftLogical %uint %147 %uint_5
        %149 = OpBitwiseOr %uint %146 %148
        %150 = OpCompositeExtract %uint %145 0
        %151 = OpShiftLeftLogical %uint %150 %uint_1
        %152 = OpBitwiseOr %uint %149 %151
        %153 = OpCompositeExtract %uint %139 0
        %154 = OpCompositeInsert %v3uint %153 %113 0
        %155 = OpCompositeExtract %uint %139 1
        %156 = OpCompositeInsert %v3uint %155 %154 1
               OpSelectionMerge %157 DontFlatten
               OpBranchConditional %94 %158 %159
        %158 = OpLabel
        %160 = OpBitcast %v3int %156
        %161 = OpShiftRightLogical %uint %103 %int_5
        %162 = OpShiftRightLogical %uint %105 %int_4
        %163 = OpCompositeExtract %int %160 2
        %164 = OpShiftRightArithmetic %int %163 %int_2
        %165 = OpBitcast %int %162
        %166 = OpIMul %int %164 %165
        %167 = OpCompositeExtract %int %160 1
        %168 = OpShiftRightArithmetic %int %167 %int_4
        %169 = OpIAdd %int %166 %168
        %170 = OpBitcast %int %161
        %171 = OpIMul %int %169 %170
        %172 = OpCompositeExtract %int %160 0
        %173 = OpShiftRightArithmetic %int %172 %int_5
        %174 = OpIAdd %int %171 %173
        %175 = OpShiftLeftLogical %int %174 %int_7
        %176 = OpBitwiseAnd %int %163 %int_3
        %177 = OpShiftLeftLogical %int %176 %int_5
        %178 = OpShiftRightArithmetic %int %167 %int_1
        %179 = OpBitwiseAnd %int %178 %int_3
        %180 = OpShiftLeftLogical %int %179 %int_3
        %181 = OpBitwiseOr %int %177 %180
        %182 = OpBitwiseAnd %int %172 %int_7
        %183 = OpBitwiseOr %int %181 %182
        %184 = OpBitwiseOr %int %175 %183
        %185 = OpShiftLeftLogical %int %184 %uint_1
        %186 = OpShiftRightArithmetic %int %167 %int_3
        %187 = OpBitwiseXor %int %186 %164
        %188 = OpBitwiseAnd %int %187 %int_1
        %189 = OpShiftRightArithmetic %int %172 %int_3
        %190 = OpBitwiseAnd %int %189 %int_3
        %191 = OpShiftLeftLogical %int %188 %int_1
        %192 = OpBitwiseXor %int %190 %191
        %193 = OpBitwiseAnd %int %167 %int_1
        %194 = OpShiftLeftLogical %int %193 %int_4
        %195 = OpShiftLeftLogical %int %192 %int_6
        %196 = OpBitwiseOr %int %194 %195
        %197 = OpShiftLeftLogical %int %188 %int_11
        %198 = OpBitwiseOr %int %196 %197
        %199 = OpBitwiseAnd %int %185 %int_15
        %200 = OpBitwiseOr %int %198 %199
        %201 = OpShiftRightArithmetic %int %185 %int_4
        %202 = OpBitwiseAnd %int %201 %int_1
        %203 = OpShiftLeftLogical %int %202 %int_5
        %204 = OpBitwiseOr %int %200 %203
        %205 = OpShiftRightArithmetic %int %185 %int_5
        %206 = OpBitwiseAnd %int %205 %int_7
        %207 = OpShiftLeftLogical %int %206 %int_8
        %208 = OpBitwiseOr %int %204 %207
        %209 = OpShiftRightArithmetic %int %185 %int_8
        %210 = OpShiftLeftLogical %int %209 %int_12
        %211 = OpBitwiseOr %int %208 %210
        %212 = OpBitcast %uint %211
               OpBranch %157
        %159 = OpLabel
        %213 = OpVectorShuffle %v2uint %156 %156 0 1
        %214 = OpBitcast %v2int %213
        %215 = OpShiftRightLogical %uint %103 %int_5
        %216 = OpCompositeExtract %int %214 1
        %217 = OpShiftRightArithmetic %int %216 %int_5
        %218 = OpBitcast %int %215
        %219 = OpIMul %int %217 %218
        %220 = OpCompositeExtract %int %214 0
        %221 = OpShiftRightArithmetic %int %220 %int_5
        %222 = OpIAdd %int %219 %221
        %223 = OpShiftLeftLogical %int %222 %int_6
        %224 = OpShiftRightArithmetic %int %216 %int_1
        %225 = OpBitwiseAnd %int %224 %int_7
        %226 = OpShiftLeftLogical %int %225 %int_3
        %227 = OpBitwiseAnd %int %220 %int_7
        %228 = OpBitwiseOr %int %226 %227
        %229 = OpBitwiseOr %int %223 %228
        %230 = OpShiftLeftLogical %int %229 %uint_1
        %231 = OpShiftRightArithmetic %int %216 %int_4
        %232 = OpBitwiseAnd %int %231 %int_1
        %233 = OpShiftRightArithmetic %int %220 %int_3
        %234 = OpBitwiseAnd %int %233 %int_3
        %235 = OpShiftRightArithmetic %int %216 %int_3
        %236 = OpBitwiseAnd %int %235 %int_1
        %237 = OpShiftLeftLogical %int %236 %int_1
        %238 = OpBitwiseXor %int %234 %237
        %239 = OpBitwiseAnd %int %216 %int_1
        %240 = OpShiftLeftLogical %int %239 %int_4
        %241 = OpShiftLeftLogical %int %238 %int_6
        %242 = OpBitwiseOr %int %240 %241
        %243 = OpShiftLeftLogical %int %232 %int_11
        %244 = OpBitwiseOr %int %242 %243
        %245 = OpBitwiseAnd %int %230 %int_15
        %246 = OpBitwiseOr %int %244 %245
        %247 = OpShiftRightArithmetic %int %230 %int_4
        %248 = OpBitwiseAnd %int %247 %int_1
        %249 = OpShiftLeftLogical %int %248 %int_5
        %250 = OpBitwiseOr %int %246 %249
        %251 = OpShiftRightArithmetic %int %230 %int_5
        %252 = OpBitwiseAnd %int %251 %int_7
        %253 = OpShiftLeftLogical %int %252 %int_8
        %254 = OpBitwiseOr %int %250 %253
        %255 = OpShiftRightArithmetic %int %230 %int_8
        %256 = OpShiftLeftLogical %int %255 %int_12
        %257 = OpBitwiseOr %int %254 %256
        %258 = OpBitcast %uint %257
               OpBranch %157
        %157 = OpLabel
        %259 = OpPhi %uint %212 %158 %258 %159
        %260 = OpCompositeExtract %uint %99 0
        %261 = OpIMul %uint %260 %141
        %262 = OpIMul %uint %259 %261
        %263 = OpIAdd %uint %262 %152
        %264 = OpIAdd %uint %263 %101
        %265 = OpShiftRightLogical %uint %264 %uint_4
        %266 = OpAccessChain %_ptr_Uniform_v4uint %xe_texture_load_source %int_0 %265
        %267 = OpLoad %v4uint %266
        %268 = OpIEqual %bool %96 %uint_1
               OpSelectionMerge %269 None
               OpBranchConditional %268 %270 %269
        %270 = OpLabel
        %271 = OpBitwiseAnd %v4uint %267 %66
        %272 = OpShiftLeftLogical %v4uint %271 %67
        %273 = OpBitwiseAnd %v4uint %267 %68
        %274 = OpShiftRightLogical %v4uint %273 %67
        %275 = OpBitwiseOr %v4uint %272 %274
               OpBranch %269
        %269 = OpLabel
        %276 = OpPhi %v4uint %267 %157 %275 %270
        %277 = OpBitwiseAnd %v4uint %276 %69
        %278 = OpShiftRightLogical %v4uint %276 %70
        %279 = OpCompositeExtract %uint %277 0
        %280 = OpCompositeExtract %uint %278 0
        %281 = OpCompositeExtract %uint %277 1
        %282 = OpCompositeExtract %uint %278 1
        %283 = OpCompositeConstruct %v4uint %279 %280 %281 %282
        %284 = OpBitwiseAnd %v4uint %283 %83
        %285 = OpBitwiseAnd %v4uint %283 %84
        %286 = OpShiftLeftLogical %v4uint %285 %76
        %287 = OpBitwiseAnd %v4uint %283 %85
        %288 = OpShiftLeftLogical %v4uint %287 %67
        %289 = OpBitwiseAnd %v4uint %283 %86
        %290 = OpShiftLeftLogical %v4uint %289 %87
        %291 = OpBitwiseOr %v4uint %284 %286
        %292 = OpBitwiseOr %v4uint %288 %290
        %293 = OpBitwiseOr %v4uint %291 %292
        %294 = OpShiftLeftLogical %v4uint %293 %76
        %295 = OpBitwiseOr %v4uint %293 %294
        %296 = OpAccessChain %_ptr_Uniform_v4uint %xe_texture_load_dest %int_0 %134
               OpStore %296 %295
        %297 = OpIAdd %uint %134 %uint_1
        %298 = OpCompositeExtract %uint %277 2
        %299 = OpCompositeExtract %uint %278 2
        %300 = OpCompositeExtract %uint %277 3
        %301 = OpCompositeExtract %uint %278 3
        %302 = OpCompositeConstruct %v4uint %298 %299 %300 %301
        %303 = OpBitwiseAnd %v4uint %302 %83
        %304 = OpBitwiseAnd %v4uint %302 %84
        %305 = OpShiftLeftLogical %v4uint %304 %76
        %306 = OpBitwiseAnd %v4uint %302 %85
        %307 = OpShiftLeftLogical %v4uint %306 %67
        %308 = OpBitwiseAnd %v4uint %302 %86
        %309 = OpShiftLeftLogical %v4uint %308 %87
        %310 = OpBitwiseOr %v4uint %303 %305
        %311 = OpBitwiseOr %v4uint %307 %309
        %312 = OpBitwiseOr %v4uint %310 %311
        %313 = OpShiftLeftLogical %v4uint %312 %76
        %314 = OpBitwiseOr %v4uint %312 %313
        %315 = OpAccessChain %_ptr_Uniform_v4uint %xe_texture_load_dest %int_0 %297
               OpStore %315 %314
        %316 = OpIAdd %uint %134 %uint_2
        %317 = OpIAdd %uint %265 %uint_1
        %318 = OpAccessChain %_ptr_Uniform_v4uint %xe_texture_load_source %int_0 %317
        %319 = OpLoad %v4uint %318
               OpSelectionMerge %320 None
               OpBranchConditional %268 %321 %320
        %321 = OpLabel
        %322 = OpBitwiseAnd %v4uint %319 %66
        %323 = OpShiftLeftLogical %v4uint %322 %67
        %324 = OpBitwiseAnd %v4uint %319 %68
        %325 = OpShiftRightLogical %v4uint %324 %67
        %326 = OpBitwiseOr %v4uint %323 %325
               OpBranch %320
        %320 = OpLabel
        %327 = OpPhi %v4uint %319 %269 %326 %321
        %328 = OpBitwiseAnd %v4uint %327 %69
        %329 = OpShiftRightLogical %v4uint %327 %70
        %330 = OpCompositeExtract %uint %328 0
        %331 = OpCompositeExtract %uint %329 0
        %332 = OpCompositeExtract %uint %328 1
        %333 = OpCompositeExtract %uint %329 1
        %334 = OpCompositeConstruct %v4uint %330 %331 %332 %333
        %335 = OpBitwiseAnd %v4uint %334 %83
        %336 = OpBitwiseAnd %v4uint %334 %84
        %337 = OpShiftLeftLogical %v4uint %336 %76
        %338 = OpBitwiseAnd %v4uint %334 %85
        %339 = OpShiftLeftLogical %v4uint %338 %67
        %340 = OpBitwiseAnd %v4uint %334 %86
        %341 = OpShiftLeftLogical %v4uint %340 %87
        %342 = OpBitwiseOr %v4uint %335 %337
        %343 = OpBitwiseOr %v4uint %339 %341
        %344 = OpBitwiseOr %v4uint %342 %343
        %345 = OpShiftLeftLogical %v4uint %344 %76
        %346 = OpBitwiseOr %v4uint %344 %345
        %347 = OpAccessChain %_ptr_Uniform_v4uint %xe_texture_load_dest %int_0 %316
               OpStore %347 %346
        %348 = OpIAdd %uint %134 %uint_3
        %349 = OpCompositeExtract %uint %328 2
        %350 = OpCompositeExtract %uint %329 2
        %351 = OpCompositeExtract %uint %328 3
        %352 = OpCompositeExtract %uint %329 3
        %353 = OpCompositeConstruct %v4uint %349 %350 %351 %352
        %354 = OpBitwiseAnd %v4uint %353 %83
        %355 = OpBitwiseAnd %v4uint %353 %84
        %356 = OpShiftLeftLogical %v4uint %355 %76
        %357 = OpBitwiseAnd %v4uint %353 %85
        %358 = OpShiftLeftLogical %v4uint %357 %67
        %359 = OpBitwiseAnd %v4uint %353 %86
        %360 = OpShiftLeftLogical %v4uint %359 %87
        %361 = OpBitwiseOr %v4uint %354 %356
        %362 = OpBitwiseOr %v4uint %358 %360
        %363 = OpBitwiseOr %v4uint %361 %362
        %364 = OpShiftLeftLogical %v4uint %363 %76
        %365 = OpBitwiseOr %v4uint %363 %364
        %366 = OpAccessChain %_ptr_Uniform_v4uint %xe_texture_load_dest %int_0 %348
               OpStore %366 %365
               OpBranch %89
         %89 = OpLabel
               OpReturn
               OpFunctionEnd
#endif

const uint32_t texture_load_r4g4b4a4_rgba8_scaled_cs[] = {
    0x07230203, 0x00010000, 0x00070000, 0x0000016F, 0x00000000, 0x00020011, 0x00000001, 0x0006000B,
    0x00000001, 0x4C534C47, 0x6474732E, 0x3035342E, 0x00000000, 0x0003000E, 0x00000000, 0x00000001,
    0x0006000F, 0x00000005, 0x00000002, 0x6E69616D, 0x00000000, 0x00000003, 0x00060010, 0x00000002,
    0x00000011, 0x00000004, 0x00000020, 0x00000001, 0x00030003, 0x00000002, 0x000001CC, 0x00090004,
    0x455F4C47, 0x635F5458, 0x72746E6F, 0x665F6C6F, 0x5F776F6C, 0x72747461, 0x74756269, 0x00007365,
    0x000B0004, 0x455F4C47, 0x735F5458, 0x6C706D61, 0x656C7265, 0x745F7373, 0x75747865, 0x665F6572,
    0x74636E75, 0x736E6F69, 0x00000000, 0x000A0004, 0x475F4C47, 0x4C474F4F, 0x70635F45, 0x74735F70,
    0x5F656C79, 0x656E696C, 0x7269645F, 0x69746365, 0x00006576, 0x00080004, 0x475F4C47, 0x4C474F4F,
    0x6E695F45, 0x64756C63, 0x69645F65, 0x74636572, 0x00657669, 0x00040005, 0x00000002, 0x6E69616D,
    0x00000000, 0x00070005, 0x00000004, 0x68737570, 0x6E6F635F, 0x625F7473, 0x6B636F6C, 0x0065785F,
    0x000E0006, 0x00000004, 0x00000000, 0x745F6578, 0x75747865, 0x6C5F6572, 0x5F64616F, 0x745F7369,
    0x64656C69, 0x5F64335F, 0x69646E65, 0x735F6E61, 0x656C6163, 0x00000000, 0x000B0006, 0x00000004,
    0x00000001, 0x745F6578, 0x75747865, 0x6C5F6572, 0x5F64616F, 0x73657567, 0x666F5F74, 0x74657366,
    0x00000000, 0x000C0006, 0x00000004, 0x00000002, 0x745F6578, 0x75747865, 0x6C5F6572, 0x5F64616F,
    0x73657567, 0x69705F74, 0x5F686374, 0x67696C61, 0x0064656E, 0x00100006, 0x00000004, 0x00000003,
    0x745F6578, 0x75747865, 0x6C5F6572, 0x5F64616F, 0x73657567, 0x5F7A5F74, 0x69727473, 0x625F6564,
    0x6B636F6C, 0x776F725F, 0x6C615F73, 0x656E6769, 0x00000064, 0x000A0006, 0x00000004, 0x00000004,
    0x745F6578, 0x75747865, 0x6C5F6572, 0x5F64616F, 0x657A6973, 0x6F6C625F, 0x00736B63, 0x000A0006,
    0x00000004, 0x00000005, 0x745F6578, 0x75747865, 0x6C5F6572, 0x5F64616F, 0x74736F68, 0x66666F5F,
    0x00746573, 0x000A0006, 0x00000004, 0x00000006, 0x745F6578, 0x75747865, 0x6C5F6572, 0x5F64616F,
    0x74736F68, 0x7469705F, 0x00006863, 0x000B0006, 0x00000004, 0x00000007, 0x745F6578, 0x75747865,
    0x6C5F6572, 0x5F64616F, 0x67696568, 0x745F7468, 0x6C657865, 0x00000073, 0x00060005, 0x00000005,
    0x68737570, 0x6E6F635F, 0x5F737473, 0x00006578, 0x00080005, 0x00000003, 0x475F6C67, 0x61626F6C,
    0x766E496C, 0x7461636F, 0x496E6F69, 0x00000044, 0x000A0005, 0x00000006, 0x745F6578, 0x75747865,
    0x6C5F6572, 0x5F64616F, 0x72756F73, 0x785F6563, 0x6C625F65, 0x006B636F, 0x00050006, 0x00000006,
    0x00000000, 0x61746164, 0x00000000, 0x00080005, 0x00000007, 0x745F6578, 0x75747865, 0x6C5F6572,
    0x5F64616F, 0x72756F73, 0x00006563, 0x000A0005, 0x00000008, 0x745F6578, 0x75747865, 0x6C5F6572,
    0x5F64616F, 0x74736564, 0x5F65785F, 0x636F6C62, 0x0000006B, 0x00050006, 0x00000008, 0x00000000,
    0x61746164, 0x00000000, 0x00080005, 0x00000009, 0x745F6578, 0x75747865, 0x6C5F6572, 0x5F64616F,
    0x74736564, 0x00000000, 0x00030047, 0x00000004, 0x00000002, 0x00050048, 0x00000004, 0x00000000,
    0x00000023, 0x00000000, 0x00050048, 0x00000004, 0x00000001, 0x00000023, 0x00000004, 0x00050048,
    0x00000004, 0x00000002, 0x00000023, 0x00000008, 0x00050048, 0x00000004, 0x00000003, 0x00000023,
    0x0000000C, 0x00050048, 0x00000004, 0x00000004, 0x00000023, 0x00000010, 0x00050048, 0x00000004,
    0x00000005, 0x00000023, 0x0000001C, 0x00050048, 0x00000004, 0x00000006, 0x00000023, 0x00000020,
    0x00050048, 0x00000004, 0x00000007, 0x00000023, 0x00000024, 0x00040047, 0x00000003, 0x0000000B,
    0x0000001C, 0x00040047, 0x0000000A, 0x00000006, 0x00000010, 0x00030047, 0x00000006, 0x00000003,
    0x00040048, 0x00000006, 0x00000000, 0x00000018, 0x00050048, 0x00000006, 0x00000000, 0x00000023,
    0x00000000, 0x00030047, 0x00000007, 0x00000018, 0x00040047, 0x00000007, 0x00000021, 0x00000000,
    0x00040047, 0x00000007, 0x00000022, 0x00000001, 0x00040047, 0x0000000B, 0x00000006, 0x00000010,
    0x00030047, 0x00000008, 0x00000003, 0x00040048, 0x00000008, 0x00000000, 0x00000019, 0x00050048,
    0x00000008, 0x00000000, 0x00000023, 0x00000000, 0x00030047, 0x00000009, 0x00000019, 0x00040047,
    0x00000009, 0x00000021, 0x00000000, 0x00040047, 0x00000009, 0x00000022, 0x00000000, 0x00040047,
    0x0000000C, 0x0000000B, 0x00000019, 0x00020013, 0x0000000D, 0x00030021, 0x0000000E, 0x0000000D,
    0x00040015, 0x0000000F, 0x00000020, 0x00000000, 0x00040017, 0x00000010, 0x0000000F, 0x00000002,
    0x00040017, 0x00000011, 0x0000000F, 0x00000004, 0x00040015, 0x00000012, 0x00000020, 0x00000001,
    0x00040017, 0x00000013, 0x00000012, 0x00000002, 0x00040017, 0x00000014, 0x00000012, 0x00000003,
    0x00020014, 0x00000015, 0x00040017, 0x00000016, 0x0000000F, 0x00000003, 0x0004002B, 0x0000000F,
    0x00000017, 0x00000001, 0x0004002B, 0x0000000F, 0x00000018, 0x00FF00FF, 0x0004002B, 0x0000000F,
    0x00000019, 0x00000008, 0x0004002B, 0x0000000F, 0x0000001A, 0xFF00FF00, 0x0004002B, 0x00000012,
    0x0000001B, 0x00000004, 0x0004002B, 0x00000012, 0x0000001C, 0x00000006, 0x0004002B, 0x00000012,
    0x0000001D, 0x0000000B, 0x0004002B, 0x00000012, 0x0000001E, 0x0000000F, 0x0004002B, 0x00000012,
    0x0000001F, 0x00000001, 0x0004002B, 0x00000012, 0x00000020, 0x00000005, 0x0004002B, 0x00000012,
    0x00000021, 0x00000007, 0x0004002B, 0x00000012, 0x00000022, 0x00000008, 0x0004002B, 0x00000012,
    0x00000023, 0x0000000C, 0x0004002B, 0x0000000F, 0x00000024, 0x00000000, 0x0004002B, 0x00000012,
    0x00000025, 0x00000003, 0x0004002B, 0x0000000F, 0x00000026, 0x00000002, 0x0004002B, 0x00000012,
    0x00000027, 0x00000002, 0x0004002B, 0x0000000F, 0x00000028, 0x00000003, 0x0004002B, 0x0000000F,
    0x00000029, 0x00000005, 0x0004002B, 0x0000000F, 0x0000002A, 0x00000004, 0x0004002B, 0x00000012,
    0x0000002B, 0x00000000, 0x000A001E, 0x00000004, 0x0000000F, 0x0000000F, 0x0000000F, 0x0000000F,
    0x00000016, 0x0000000F, 0x0000000F, 0x0000000F, 0x00040020, 0x0000002C, 0x00000009, 0x00000004,
    0x0004003B, 0x0000002C, 0x00000005, 0x00000009, 0x00040020, 0x0000002D, 0x00000009, 0x0000000F,
    0x0004002B, 0x0000000F, 0x0000002E, 0x00000007, 0x0005002C, 0x00000010, 0x0000002F, 0x0000002A,
    0x0000002E, 0x00040020, 0x00000030, 0x00000009, 0x00000016, 0x0004002B, 0x0000000F, 0x00000031,
    0x0000001F, 0x0004002B, 0x0000000F, 0x00000032, 0x0000003F, 0x0004002B, 0x0000000F, 0x00000033,
    0x0000000B, 0x0004002B, 0x0000000F, 0x00000034, 0x00000010, 0x0004002B, 0x0000000F, 0x00000035,
    0x00000018, 0x00040020, 0x00000036, 0x00000001, 0x00000016, 0x0004003B, 0x00000036, 0x00000003,
    0x00000001, 0x0006002C, 0x00000016, 0x00000037, 0x0000002A, 0x00000024, 0x00000024, 0x00040017,
    0x00000038, 0x00000015, 0x00000002, 0x0003001D, 0x0000000A, 0x00000011, 0x0003001E, 0x00000006,
    0x0000000A, 0x00040020, 0x00000039, 0x00000002, 0x00000006, 0x0004003B, 0x00000039, 0x00000007,
    0x00000002, 0x00040020, 0x0000003A, 0x00000002, 0x00000011, 0x0004002B, 0x0000000F, 0x0000003B,
    0x0000FFFF, 0x0003001D, 0x0000000B, 0x00000011, 0x0003001E, 0x00000008, 0x0000000B, 0x00040020,
    0x0000003C, 0x00000002, 0x00000008, 0x0004003B, 0x0000003C, 0x00000009, 0x00000002, 0x0004002B,
    0x0000000F, 0x0000003D, 0x00000020, 0x0006002C, 0x00000016, 0x0000000C, 0x0000002A, 0x0000003D,
    0x00000017, 0x0005002C, 0x00000010, 0x0000003E, 0x0000002E, 0x0000002E, 0x0005002C, 0x00000010,
    0x0000003F, 0x0000002A, 0x00000026, 0x0004002B, 0x0000000F, 0x00000040, 0x0000000F, 0x0005002C,
    0x00000010, 0x00000041, 0x00000040, 0x00000028, 0x0007002C, 0x00000011, 0x00000042, 0x00000018,
    0x00000018, 0x00000018, 0x00000018, 0x0007002C, 0x00000011, 0x00000043, 0x00000019, 0x00000019,
    0x00000019, 0x00000019, 0x0007002C, 0x00000011, 0x00000044, 0x0000001A, 0x0000001A, 0x0000001A,
    0x0000001A, 0x0007002C, 0x00000011, 0x00000045, 0x0000003B, 0x0000003B, 0x0000003B, 0x0000003B,
    0x0007002C, 0x00000011, 0x00000046, 0x00000034, 0x00000034, 0x00000034, 0x00000034, 0x0007002C,
    0x00000011, 0x00000047, 0x00000031, 0x00000031, 0x00000031, 0x00000031, 0x0007002C, 0x00000011,
    0x00000048, 0x00000028, 0x00000028, 0x00000028, 0x00000028, 0x0007002C, 0x00000011, 0x00000049,
    0x00000026, 0x00000026, 0x00000026, 0x00000026, 0x0007002C, 0x00000011, 0x0000004A, 0x00000029,
    0x00000029, 0x00000029, 0x00000029, 0x0007002C, 0x00000011, 0x0000004B, 0x00000032, 0x00000032,
    0x00000032, 0x00000032, 0x0007002C, 0x00000011, 0x0000004C, 0x0000002A, 0x0000002A, 0x0000002A,
    0x0000002A, 0x0007002C, 0x00000011, 0x0000004D, 0x00000033, 0x00000033, 0x00000033, 0x00000033,
    0x0007002C, 0x00000011, 0x0000004E, 0x00000035, 0x00000035, 0x00000035, 0x00000035, 0x0004002B,
    0x0000000F, 0x0000004F, 0x000000F0, 0x0004002B, 0x0000000F, 0x00000050, 0x00000F00, 0x0004002B,
    0x0000000F, 0x00000051, 0x0000F000, 0x0004002B, 0x0000000F, 0x00000052, 0x0000000C, 0x0007002C,
    0x00000011, 0x00000053, 0x00000040, 0x00000040, 0x00000040, 0x00000040, 0x0007002C, 0x00000011,
    0x00000054, 0x0000004F, 0x0000004F, 0x0000004F, 0x0000004F, 0x0007002C, 0x00000011, 0x00000055,
    0x00000050, 0x00000050, 0x00000050, 0x00000050, 0x0007002C, 0x00000011, 0x00000056, 0x00000051,
    0x00000051, 0x00000051, 0x00000051, 0x0007002C, 0x00000011, 0x00000057, 0x00000052, 0x00000052,
    0x00000052, 0x00000052, 0x00050036, 0x0000000D, 0x00000002, 0x00000000, 0x0000000E, 0x000200F8,
    0x00000058, 0x000300F7, 0x00000059, 0x00000000, 0x000300FB, 0x00000024, 0x0000005A, 0x000200F8,
    0x0000005A, 0x00050041, 0x0000002D, 0x0000005B, 0x00000005, 0x0000002B, 0x0004003D, 0x0000000F,
    0x0000005C, 0x0000005B, 0x000500C7, 0x0000000F, 0x0000005D, 0x0000005C, 0x00000026, 0x000500AB,
    0x00000015, 0x0000005E, 0x0000005D, 0x00000024, 0x000500C2, 0x0000000F, 0x0000005F, 0x0000005C,
    0x00000026, 0x000500C7, 0x0000000F, 0x00000060, 0x0000005F, 0x00000028, 0x00050050, 0x00000010,
    0x00000061, 0x0000005C, 0x0000005C, 0x000500C2, 0x00000010, 0x00000062, 0x00000061, 0x0000002F,
    0x000500C7, 0x00000010, 0x00000063, 0x00000062, 0x0000003E, 0x00050041, 0x0000002D, 0x00000064,
    0x00000005, 0x0000001F, 0x0004003D, 0x0000000F, 0x00000065, 0x00000064, 0x00050041, 0x0000002D,
    0x00000066, 0x00000005, 0x00000027, 0x0004003D, 0x0000000F, 0x00000067, 0x00000066, 0x00050041,
    0x0000002D, 0x00000068, 0x00000005, 0x00000025, 0x0004003D, 0x0000000F, 0x00000069, 0x00000068,
    0x00050041, 0x00000030, 0x0000006A, 0x00000005, 0x0000001B, 0x0004003D, 0x00000016, 0x0000006B,
    0x0000006A, 0x00050041, 0x0000002D, 0x0000006C, 0x00000005, 0x00000020, 0x0004003D, 0x0000000F,
    0x0000006D, 0x0000006C, 0x00050041, 0x0000002D, 0x0000006E, 0x00000005, 0x0000001C, 0x0004003D,
    0x0000000F, 0x0000006F, 0x0000006E, 0x0004003D, 0x00000016, 0x00000070, 0x00000003, 0x000500C4,
    0x00000016, 0x00000071, 0x00000070, 0x00000037, 0x0007004F, 0x00000010, 0x00000072, 0x00000071,
    0x00000071, 0x00000000, 0x00000001, 0x0007004F, 0x00000010, 0x00000073, 0x0000006B, 0x0000006B,
    0x00000000, 0x00000001, 0x000500AE, 0x00000038, 0x00000074, 0x00000072, 0x00000073, 0x0004009A,
    0x00000015, 0x00000075, 0x00000074, 0x000300F7, 0x00000076, 0x00000002, 0x000400FA, 0x00000075,
    0x00000077, 0x00000076, 0x000200F8, 0x00000077, 0x000200F9, 0x00000059, 0x000200F8, 0x00000076,
    0x0004007C, 0x00000014, 0x00000078, 0x00000071, 0x00050051, 0x0000000F, 0x00000079, 0x0000006B,
    0x00000001, 0x00050051, 0x00000012, 0x0000007A, 0x00000078, 0x00000000, 0x00050084, 0x00000012,
    0x0000007B, 0x0000007A, 0x0000001B, 0x00050051, 0x00000012, 0x0000007C, 0x00000078, 0x00000002,
    0x0004007C, 0x00000012, 0x0000007D, 0x00000079, 0x00050084, 0x00000012, 0x0000007E, 0x0000007C,
    0x0000007D, 0x00050051, 0x00000012, 0x0000007F, 0x00000078, 0x00000001, 0x00050080, 0x00000012,
    0x00000080, 0x0000007E, 0x0000007F, 0x0004007C, 0x00000012, 0x00000081, 0x0000006F, 0x00050084,
    0x00000012, 0x00000082, 0x00000080, 0x00000081, 0x00050080, 0x00000012, 0x00000083, 0x0000007B,
    0x00000082, 0x0004007C, 0x0000000F, 0x00000084, 0x00000083, 0x00050080, 0x0000000F, 0x00000085,
    0x00000084, 0x0000006D, 0x000500C2, 0x0000000F, 0x00000086, 0x00000085, 0x0000002A, 0x000500C2,
    0x00000010, 0x00000087, 0x00000072, 0x0000003F, 0x00050086, 0x00000010, 0x00000088, 0x00000087,
    0x00000063, 0x00050084, 0x00000010, 0x00000089, 0x00000063, 0x00000088, 0x00050082, 0x00000010,
    0x0000008A, 0x00000087, 0x00000089, 0x000500C4, 0x00000010, 0x0000008B, 0x00000088, 0x0000003F,
    0x00050051, 0x0000000F, 0x0000008C, 0x0000008A, 0x00000000, 0x00050051, 0x0000000F, 0x0000008D,
    0x00000063, 0x00000001, 0x00050084, 0x0000000F, 0x0000008E, 0x0000008C, 0x0000008D, 0x00050051,
    0x0000000F, 0x0000008F, 0x0000008A, 0x00000001, 0x00050080, 0x0000000F, 0x00000090, 0x0000008E,
    0x0000008F, 0x000500C7, 0x00000010, 0x00000091, 0x00000072, 0x00000041, 0x000500C4, 0x0000000F,
    0x00000092, 0x00000090, 0x0000002E, 0x00050051, 0x0000000F, 0x00000093, 0x00000091, 0x00000001,
    0x000500C4, 0x0000000F, 0x00000094, 0x00000093, 0x00000029, 0x000500C5, 0x0000000F, 0x00000095,
    0x00000092, 0x00000094, 0x00050051, 0x0000000F, 0x00000096, 0x00000091, 0x00000000, 0x000500C4,
    0x0000000F, 0x00000097, 0x00000096, 0x00000017, 0x000500C5, 0x0000000F, 0x00000098, 0x00000095,
    0x00000097, 0x00050051, 0x0000000F, 0x00000099, 0x0000008B, 0x00000000, 0x00060052, 0x00000016,
    0x0000009A, 0x00000099, 0x00000071, 0x00000000, 0x00050051, 0x0000000F, 0x0000009B, 0x0000008B,
    0x00000001, 0x00060052, 0x00000016, 0x0000009C, 0x0000009B, 0x0000009A, 0x00000001, 0x000300F7,
    0x0000009D, 0x00000002, 0x000400FA, 0x0000005E, 0x0000009E, 0x0000009F, 0x000200F8, 0x0000009E,
    0x0004007C, 0x00000014, 0x000000A0, 0x0000009C, 0x000500C2, 0x0000000F, 0x000000A1, 0x00000067,
    0x00000020, 0x000500C2, 0x0000000F, 0x000000A2, 0x00000069, 0x0000001B, 0x00050051, 0x00000012,
    0x000000A3, 0x000000A0, 0x00000002, 0x000500C3, 0x00000012, 0x000000A4, 0x000000A3, 0x00000027,
    0x0004007C, 0x00000012, 0x000000A5, 0x000000A2, 0x00050084, 0x00000012, 0x000000A6, 0x000000A4,
    0x000000A5, 0x00050051, 0x00000012, 0x000000A7, 0x000000A0, 0x00000001, 0x000500C3, 0x00000012,
    0x000000A8, 0x000000A7, 0x0000001B, 0x00050080, 0x00000012, 0x000000A9, 0x000000A6, 0x000000A8,
    0x0004007C, 0x00000012, 0x000000AA, 0x000000A1, 0x00050084, 0x00000012, 0x000000AB, 0x000000A9,
    0x000000AA, 0x00050051, 0x00000012, 0x000000AC, 0x000000A0, 0x00000000, 0x000500C3, 0x00000012,
    0x000000AD, 0x000000AC, 0x00000020, 0x00050080, 0x00000012, 0x000000AE, 0x000000AB, 0x000000AD,
    0x000500C4, 0x00000012, 0x000000AF, 0x000000AE, 0x00000021, 0x000500C7, 0x00000012, 0x000000B0,
    0x000000A3, 0x00000025, 0x000500C4, 0x00000012, 0x000000B1, 0x000000B0, 0x00000020, 0x000500C3,
    0x00000012, 0x000000B2, 0x000000A7, 0x0000001F, 0x000500C7, 0x00000012, 0x000000B3, 0x000000B2,
    0x00000025, 0x000500C4, 0x00000012, 0x000000B4, 0x000000B3, 0x00000025, 0x000500C5, 0x00000012,
    0x000000B5, 0x000000B1, 0x000000B4, 0x000500C7, 0x00000012, 0x000000B6, 0x000000AC, 0x00000021,
    0x000500C5, 0x00000012, 0x000000B7, 0x000000B5, 0x000000B6, 0x000500C5, 0x00000012, 0x000000B8,
    0x000000AF, 0x000000B7, 0x000500C4, 0x00000012, 0x000000B9, 0x000000B8, 0x00000017, 0x000500C3,
    0x00000012, 0x000000BA, 0x000000A7, 0x00000025, 0x000500C6, 0x00000012, 0x000000BB, 0x000000BA,
    0x000000A4, 0x000500C7, 0x00000012, 0x000000BC, 0x000000BB, 0x0000001F, 0x000500C3, 0x00000012,
    0x000000BD, 0x000000AC, 0x00000025, 0x000500C7, 0x00000012, 0x000000BE, 0x000000BD, 0x00000025,
    0x000500C4, 0x00000012, 0x000000BF, 0x000000BC, 0x0000001F, 0x000500C6, 0x00000012, 0x000000C0,
    0x000000BE, 0x000000BF, 0x000500C7, 0x00000012, 0x000000C1, 0x000000A7, 0x0000001F, 0x000500C4,
    0x00000012, 0x000000C2, 0x000000C1, 0x0000001B, 0x000500C4, 0x00000012, 0x000000C3, 0x000000C0,
    0x0000001C, 0x000500C5, 0x00000012, 0x000000C4, 0x000000C2, 0x000000C3, 0x000500C4, 0x00000012,
    0x000000C5, 0x000000BC, 0x0000001D, 0x000500C5, 0x00000012, 0x000000C6, 0x000000C4, 0x000000C5,
    0x000500C7, 0x00000012, 0x000000C7, 0x000000B9, 0x0000001E, 0x000500C5, 0x00000012, 0x000000C8,
    0x000000C6, 0x000000C7, 0x000500C3, 0x00000012, 0x000000C9, 0x000000B9, 0x0000001B, 0x000500C7,
    0x00000012, 0x000000CA, 0x000000C9, 0x0000001F, 0x000500C4, 0x00000012, 0x000000CB, 0x000000CA,
    0x00000020, 0x000500C5, 0x00000012, 0x000000CC, 0x000000C8, 0x000000CB, 0x000500C3, 0x00000012,
    0x000000CD, 0x000000B9, 0x00000020, 0x000500C7, 0x00000012, 0x000000CE, 0x000000CD, 0x00000021,
    0x000500C4, 0x00000012, 0x000000CF, 0x000000CE, 0x00000022, 0x000500C5, 0x00000012, 0x000000D0,
    0x000000CC, 0x000000CF, 0x000500C3, 0x00000012, 0x000000D1, 0x000000B9, 0x00000022, 0x000500C4,
    0x00000012, 0x000000D2, 0x000000D1, 0x00000023, 0x000500C5, 0x00000012, 0x000000D3, 0x000000D0,
    0x000000D2, 0x0004007C, 0x0000000F, 0x000000D4, 0x000000D3, 0x000200F9, 0x0000009D, 0x000200F8,
    0x0000009F, 0x0007004F, 0x00000010, 0x000000D5, 0x0000009C, 0x0000009C, 0x00000000, 0x00000001,
    0x0004007C, 0x00000013, 0x000000D6, 0x000000D5, 0x000500C2, 0x0000000F, 0x000000D7, 0x00000067,
    0x00000020, 0x00050051, 0x00000012, 0x000000D8, 0x000000D6, 0x00000001, 0x000500C3, 0x00000012,
    0x000000D9, 0x000000D8, 0x00000020, 0x0004007C, 0x00000012, 0x000000DA, 0x000000D7, 0x00050084,
    0x00000012, 0x000000DB, 0x000000D9, 0x000000DA, 0x00050051, 0x00000012, 0x000000DC, 0x000000D6,
    0x00000000, 0x000500C3, 0x00000012, 0x000000DD, 0x000000DC, 0x00000020, 0x00050080, 0x00000012,
    0x000000DE, 0x000000DB, 0x000000DD, 0x000500C4, 0x00000012, 0x000000DF, 0x000000DE, 0x0000001C,
    0x000500C3, 0x00000012, 0x000000E0, 0x000000D8, 0x0000001F, 0x000500C7, 0x00000012, 0x000000E1,
    0x000000E0, 0x00000021, 0x000500C4, 0x00000012, 0x000000E2, 0x000000E1, 0x00000025, 0x000500C7,
    0x00000012, 0x000000E3, 0x000000DC, 0x00000021, 0x000500C5, 0x00000012, 0x000000E4, 0x000000E2,
    0x000000E3, 0x000500C5, 0x00000012, 0x000000E5, 0x000000DF, 0x000000E4, 0x000500C4, 0x00000012,
    0x000000E6, 0x000000E5, 0x00000017, 0x000500C3, 0x00000012, 0x000000E7, 0x000000D8, 0x0000001B,
    0x000500C7, 0x00000012, 0x000000E8, 0x000000E7, 0x0000001F, 0x000500C3, 0x00000012, 0x000000E9,
    0x000000DC, 0x00000025, 0x000500C7, 0x00000012, 0x000000EA, 0x000000E9, 0x00000025, 0x000500C3,
    0x00000012, 0x000000EB, 0x000000D8, 0x00000025, 0x000500C7, 0x00000012, 0x000000EC, 0x000000EB,
    0x0000001F, 0x000500C4, 0x00000012, 0x000000ED, 0x000000EC, 0x0000001F, 0x000500C6, 0x00000012,
    0x000000EE, 0x000000EA, 0x000000ED, 0x000500C7, 0x00000012, 0x000000EF, 0x000000D8, 0x0000001F,
    0x000500C4, 0x00000012, 0x000000F0, 0x000000EF, 0x0000001B, 0x000500C4, 0x00000012, 0x000000F1,
    0x000000EE, 0x0000001C, 0x000500C5, 0x00000012, 0x000000F2, 0x000000F0, 0x000000F1, 0x000500C4,
    0x00000012, 0x000000F3, 0x000000E8, 0x0000001D, 0x000500C5, 0x00000012, 0x000000F4, 0x000000F2,
    0x000000F3, 0x000500C7, 0x00000012, 0x000000F5, 0x000000E6, 0x0000001E, 0x000500C5, 0x00000012,
    0x000000F6, 0x000000F4, 0x000000F5, 0x000500C3, 0x00000012, 0x000000F7, 0x000000E6, 0x0000001B,
    0x000500C7, 0x00000012, 0x000000F8, 0x000000F7, 0x0000001F, 0x000500C4, 0x00000012, 0x000000F9,
    0x000000F8, 0x00000020, 0x000500C5, 0x00000012, 0x000000FA, 0x000000F6, 0x000000F9, 0x000500C3,
    0x00000012, 0x000000FB, 0x000000E6, 0x00000020, 0x000500C7, 0x00000012, 0x000000FC, 0x000000FB,
    0x00000021, 0x000500C4, 0x00000012, 0x000000FD, 0x000000FC, 0x00000022, 0x000500C5, 0x00000012,
    0x000000FE, 0x000000FA, 0x000000FD, 0x000500C3, 0x00000012, 0x000000FF, 0x000000E6, 0x00000022,
    0x000500C4, 0x00000012, 0x00000100, 0x000000FF, 0x00000023, 0x000500C5, 0x00000012, 0x00000101,
    0x000000FE, 0x00000100, 0x0004007C, 0x0000000F, 0x00000102, 0x00000101, 0x000200F9, 0x0000009D,
    0x000200F8, 0x0000009D, 0x000700F5, 0x0000000F, 0x00000103, 0x000000D4, 0x0000009E, 0x00000102,
    0x0000009F, 0x00050051, 0x0000000F, 0x00000104, 0x00000063, 0x00000000, 0x00050084, 0x0000000F,
    0x00000105, 0x00000104, 0x0000008D, 0x00050084, 0x0000000F, 0x00000106, 0x00000103, 0x00000105,
    0x00050080, 0x0000000F, 0x00000107, 0x00000106, 0x00000098, 0x00050080, 0x0000000F, 0x00000108,
    0x00000107, 0x00000065, 0x000500C2, 0x0000000F, 0x00000109, 0x00000108, 0x0000002A, 0x00060041,
    0x0000003A, 0x0000010A, 0x00000007, 0x0000002B, 0x00000109, 0x0004003D, 0x00000011, 0x0000010B,
    0x0000010A, 0x000500AA, 0x00000015, 0x0000010C, 0x00000060, 0x00000017, 0x000300F7, 0x0000010D,
    0x00000000, 0x000400FA, 0x0000010C, 0x0000010E, 0x0000010D, 0x000200F8, 0x0000010E, 0x000500C7,
    0x00000011, 0x0000010F, 0x0000010B, 0x00000042, 0x000500C4, 0x00000011, 0x00000110, 0x0000010F,
    0x00000043, 0x000500C7, 0x00000011, 0x00000111, 0x0000010B, 0x00000044, 0x000500C2, 0x00000011,
    0x00000112, 0x00000111, 0x00000043, 0x000500C5, 0x00000011, 0x00000113, 0x00000110, 0x00000112,
    0x000200F9, 0x0000010D, 0x000200F8, 0x0000010D, 0x000700F5, 0x00000011, 0x00000114, 0x0000010B,
    0x0000009D, 0x00000113, 0x0000010E, 0x000500C7, 0x00000011, 0x00000115, 0x00000114, 0x00000045,
    0x000500C2, 0x00000011, 0x00000116, 0x00000114, 0x00000046, 0x00050051, 0x0000000F, 0x00000117,
    0x00000115, 0x00000000, 0x00050051, 0x0000000F, 0x00000118, 0x00000116, 0x00000000, 0x00050051,
    0x0000000F, 0x00000119, 0x00000115, 0x00000001, 0x00050051, 0x0000000F, 0x0000011A, 0x00000116,
    0x00000001, 0x00070050, 0x00000011, 0x0000011B, 0x00000117, 0x00000118, 0x00000119, 0x0000011A,
    0x000500C7, 0x00000011, 0x0000011C, 0x0000011B, 0x00000053, 0x000500C7, 0x00000011, 0x0000011D,
    0x0000011B, 0x00000054, 0x000500C4, 0x00000011, 0x0000011E, 0x0000011D, 0x0000004C, 0x000500C7,
    0x00000011, 0x0000011F, 0x0000011B, 0x00000055, 0x000500C4, 0x00000011, 0x00000120, 0x0000011F,
    0x00000043, 0x000500C7, 0x00000011, 0x00000121, 0x0000011B, 0x00000056, 0x000500C4, 0x00000011,
    0x00000122, 0x00000121, 0x00000057, 0x000500C5, 0x00000011, 0x00000123, 0x0000011C, 0x0000011E,
    0x000500C5, 0x00000011, 0x00000124, 0x00000120, 0x00000122, 0x000500C5, 0x00000011, 0x00000125,
    0x00000123, 0x00000124, 0x000500C4, 0x00000011, 0x00000126, 0x00000125, 0x0000004C, 0x000500C5,
    0x00000011, 0x00000127, 0x00000125, 0x00000126, 0x00060041, 0x0000003A, 0x00000128, 0x00000009,
    0x0000002B, 0x00000086, 0x0003003E, 0x00000128, 0x00000127, 0x00050080, 0x0000000F, 0x00000129,
    0x00000086, 0x00000017, 0x00050051, 0x0000000F, 0x0000012A, 0x00000115, 0x00000002, 0x00050051,
    0x0000000F, 0x0000012B, 0x00000116, 0x00000002, 0x00050051, 0x0000000F, 0x0000012C, 0x00000115,
    0x00000003, 0x00050051, 0x0000000F, 0x0000012D, 0x00000116, 0x00000003, 0x00070050, 0x00000011,
    0x0000012E, 0x0000012A, 0x0000012B, 0x0000012C, 0x0000012D, 0x000500C7, 0x00000011, 0x0000012F,
    0x0000012E, 0x00000053, 0x000500C7, 0x00000011, 0x00000130, 0x0000012E, 0x00000054, 0x000500C4,
    0x00000011, 0x00000131, 0x00000130, 0x0000004C, 0x000500C7, 0x00000011, 0x00000132, 0x0000012E,
    0x00000055, 0x000500C4, 0x00000011, 0x00000133, 0x00000132, 0x00000043, 0x000500C7, 0x00000011,
    0x00000134, 0x0000012E, 0x00000056, 0x000500C4, 0x00000011, 0x00000135, 0x00000134, 0x00000057,
    0x000500C5, 0x00000011, 0x00000136, 0x0000012F, 0x00000131, 0x000500C5, 0x00000011, 0x00000137,
    0x00000133, 0x00000135, 0x000500C5, 0x00000011, 0x00000138, 0x00000136, 0x00000137, 0x000500C4,
    0x00000011, 0x00000139, 0x00000138, 0x0000004C, 0x000500C5, 0x00000011, 0x0000013A, 0x00000138,
    0x00000139, 0x00060041, 0x0000003A, 0x0000013B, 0x00000009, 0x0000002B, 0x00000129, 0x0003003E,
    0x0000013B, 0x0000013A, 0x00050080, 0x0000000F, 0x0000013C, 0x00000086, 0x00000026, 0x00050080,
    0x0000000F, 0x0000013D, 0x00000109, 0x00000017, 0x00060041, 0x0000003A, 0x0000013E, 0x00000007,
    0x0000002B, 0x0000013D, 0x0004003D, 0x00000011, 0x0000013F, 0x0000013E, 0x000300F7, 0x00000140,
    0x00000000, 0x000400FA, 0x0000010C, 0x00000141, 0x00000140, 0x000200F8, 0x00000141, 0x000500C7,
    0x00000011, 0x00000142, 0x0000013F, 0x00000042, 0x000500C4, 0x00000011, 0x00000143, 0x00000142,
    0x00000043, 0x000500C7, 0x00000011, 0x00000144, 0x0000013F, 0x00000044, 0x000500C2, 0x00000011,
    0x00000145, 0x00000144, 0x00000043, 0x000500C5, 0x00000011, 0x00000146, 0x00000143, 0x00000145,
    0x000200F9, 0x00000140, 0x000200F8, 0x00000140, 0x000700F5, 0x00000011, 0x00000147, 0x0000013F,
    0x0000010D, 0x00000146, 0x00000141, 0x000500C7, 0x00000011, 0x00000148, 0x00000147, 0x00000045,
    0x000500C2, 0x00000011, 0x00000149, 0x00000147, 0x00000046, 0x00050051, 0x0000000F, 0x0000014A,
    0x00000148, 0x00000000, 0x00050051, 0x0000000F, 0x0000014B, 0x00000149, 0x00000000, 0x00050051,
    0x0000000F, 0x0000014C, 0x00000148, 0x00000001, 0x00050051, 0x0000000F, 0x0000014D, 0x00000149,
    0x00000001, 0x00070050, 0x00000011, 0x0000014E, 0x0000014A, 0x0000014B, 0x0000014C, 0x0000014D,
    0x000500C7, 0x00000011, 0x0000014F, 0x0000014E, 0x00000053, 0x000500C7, 0x00000011, 0x00000150,
    0x0000014E, 0x00000054, 0x000500C4, 0x00000011, 0x00000151, 0x00000150, 0x0000004C, 0x000500C7,
    0x00000011, 0x00000152, 0x0000014E, 0x00000055, 0x000500C4, 0x00000011, 0x00000153, 0x00000152,
    0x00000043, 0x000500C7, 0x00000011, 0x00000154, 0x0000014E, 0x00000056, 0x000500C4, 0x00000011,
    0x00000155, 0x00000154, 0x00000057, 0x000500C5, 0x00000011, 0x00000156, 0x0000014F, 0x00000151,
    0x000500C5, 0x00000011, 0x00000157, 0x00000153, 0x00000155, 0x000500C5, 0x00000011, 0x00000158,
    0x00000156, 0x00000157, 0x000500C4, 0x00000011, 0x00000159, 0x00000158, 0x0000004C, 0x000500C5,
    0x00000011, 0x0000015A, 0x00000158, 0x00000159, 0x00060041, 0x0000003A, 0x0000015B, 0x00000009,
    0x0000002B, 0x0000013C, 0x0003003E, 0x0000015B, 0x0000015A, 0x00050080, 0x0000000F, 0x0000015C,
    0x00000086, 0x00000028, 0x00050051, 0x0000000F, 0x0000015D, 0x00000148, 0x00000002, 0x00050051,
    0x0000000F, 0x0000015E, 0x00000149, 0x00000002, 0x00050051, 0x0000000F, 0x0000015F, 0x00000148,
    0x00000003, 0x00050051, 0x0000000F, 0x00000160, 0x00000149, 0x00000003, 0x00070050, 0x00000011,
    0x00000161, 0x0000015D, 0x0000015E, 0x0000015F, 0x00000160, 0x000500C7, 0x00000011, 0x00000162,
    0x00000161, 0x00000053, 0x000500C7, 0x00000011, 0x00000163, 0x00000161, 0x00000054, 0x000500C4,
    0x00000011, 0x00000164, 0x00000163, 0x0000004C, 0x000500C7, 0x00000011, 0x00000165, 0x00000161,
    0x00000055, 0x000500C4, 0x00000011, 0x00000166, 0x00000165, 0x00000043, 0x000500C7, 0x00000011,
    0x00000167, 0x00000161, 0x00000056, 0x000500C4, 0x00000011, 0x00000168, 0x00000167, 0x00000057,
    0x000500C5, 0x00000011, 0x00000169, 0x00000162, 0x00000164, 0x000500C5, 0x00000011, 0x0000016A,
    0x00000166, 0x00000168, 0x000500C5, 0x00000011, 0x0000016B, 0x00000169, 0x0000016A, 0x000500C4,
    0x00000011, 0x0000016C, 0x0000016B, 0x0000004C, 0x000500C5, 0x00000011, 0x0000016D, 0x0000016B,
    0x0000016C, 0x00060041, 0x0000003A, 0x0000016E, 0x00000009, 0x0000002B, 0x0000015C, 0x0003003E,
    0x0000016E, 0x0000016D, 0x000200F9, 0x00000059, 0x000200F8, 0x00000059, 0x000100FD, 0x00010038,
};
