// MIT License
// 
// Copyright (C) 2018-2024, Tellusim Technologies Inc. https://tellusim.com/
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#version 420 core

#if COMPUTE_SHADER
	
	layout(local_size_x = 8, local_size_y = 8) in;
	
	layout(binding = 0, set = 0) uniform texture2D in_texture_0;
	layout(binding = 1, set = 0) uniform texture2D in_texture_1;
	layout(binding = 0, set = 1) uniform sampler in_sampler;
	
	layout(binding = 2, set = 0, rgba8) uniform writeonly image2D out_surface;
	
	/*
	 */
	void main() {
		
		ivec2 global_id = ivec2(gl_GlobalInvocationID.xy);
		[[branch]] if(global_id.x >= 224 || global_id.y >= 224) return;
		
		float iaspect = 720.0f / 1280.0f;
		vec2 texcoord = vec2(global_id) * vec2(iaspect, 1.0f) / 224.0f + vec2(0.25f, 0.0f);
		
		float Y = texture(sampler2D(in_texture_0, in_sampler), texcoord).x * 1.164f - 0.07275f;
		vec2 UV = texture(sampler2D(in_texture_1, in_sampler), texcoord).xy - 0.5f;
		
		float r = Y + 1.596f * UV.y;
		float g = Y - 0.813f * UV.y - 0.391f * UV.x;
		float b = Y + 2.018f * UV.x;
		
		#if _IOS
			global_id = ivec2(global_id.y, 223 - global_id.x);
		#endif
		
		imageStore(out_surface, global_id, vec4(r, g, b, 1.0f));
	}
	
#endif
