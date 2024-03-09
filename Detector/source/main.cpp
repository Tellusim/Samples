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

#include <TellusimApp.h>
#include <core/TellusimLog.h>
#include <core/TellusimSource.h>
#include <core/TellusimPointer.h>
#include <platform/TellusimDevice.h>
#include <platform/TellusimSampler.h>
#include <platform/TellusimKernel.h>
#include <platform/TellusimCommand.h>
#include <platform/TellusimCompute.h>
#include <platform/TellusimWindow.h>
#include <interface/TellusimCanvas.h>
#include <interface/TellusimControls.h>

#include "Panel.h"
#include "Plugins.h"

/*
 */
#ifndef DATA_PATH
	#define DATA_PATH	"../data/"
#endif

/*
 */
using namespace Tellusim;

/*
 */
class Detector {
		
	public:
		
		explicit Detector(App &app) : app(app) {
			
		}
		virtual ~Detector() {
			
		}
		
		// create app
		bool create(const char *title) {
			
			// create window
			window = Window(app.getPlatform(), app.getDevice());
			if(!window || !window.setSize(app.getWidth(), app.getHeight())) return false;
			if(!window.create(title) || !window.setHidden(false)) return false;
			window.setKeyboardPressedCallback([&](uint32_t key, uint32_t code) {
				if(key == Window::KeyEsc) window.stop();
			});
			
			// create device
			device = Device(window);
			if(!device) return false;
			
			// check compute shader support
			if(!device.hasShader(Shader::TypeCompute)) {
				TS_LOG(Error, "compute shader is not supported\n");
				return 0;
			}
			
			// create target
			target = device.createTarget(window);
			if(!target) return false;
			
			// shader cache
			Shader::setCache("main.cache");
			
			// create capture
			if(!capture.setSize(1280, 720)) return false;
			
			// open capture
			if(!capture.setFlip(false, true)) return false;
			if(!capture.open(Capture::FlagCapturePreview)) {
				TS_LOG(Error, "can't open capture\n");
				return false;
			}
			
			// capture info
			TS_LOGF(Message, "%s %s (%s)\n", capture.getTypeName(), capture.getName().get(), capture.getFlagsName().get());
			
			// create panel
			panel = makeAutoPtr(new Panel(device));
			#if _IOS
				panel->getPanel().setPosition(0.0f, -48.0f);
			#endif
			panel->getInfoText().setFontSize(24);
			
			// create texture rect
			texture_rect = ControlRect(&panel->getRoot());
			texture_rect.setMode(CanvasElement::ModeTexture);
			texture_rect.setAlign(Control::AlignCenter | Control::AlignOverlap);
			panel->getRoot().lowerChild(texture_rect);
			
			// load classes
			Source source;
			if(!source.open(DATA_PATH "imagenet_classes.txt")) {
				TS_LOG(Error, "can't open open classes\n");
				return false;
			}
			while(source.isAvailable()) {
				classes.append(source.readLine());
			}
			TS_LOGF(Message, "%u classes\n", classes.size());
			
			// create kernel
			kernel = device.createKernel().setSamplers(1).setTextures(2).setSurfaces(1);
			if(!kernel.loadShaderGLSL(DATA_PATH "main.shader", "COMPUTE_SHADER=1")) return false;
			if(!kernel.create()) return false;
			
			// create sampler
			sampler = device.createSampler(Sampler::FilterLinear, Sampler::WrapModeClamp);
			if(!sampler) return false;
			
			// create texture
			constexpr uint32_t size = 224;
			rgb_texture = device.createTexture2D(FormatRGBAu8n, size, size, Texture::FlagSurface);
			if(!rgb_texture) return false;
			
			// create capture textures
			luma_texture = device.createTexture2D(FormatRu8n, capture.getWidth(), capture.getHeight());
			chroma_texture = device.createTexture2D(FormatRGu8n, capture.getWidth() / 2, capture.getHeight() / 2);
			if(!luma_texture || !chroma_texture) return false;
			
			// create tensor graph
			if(!tensor_graph.create(device, TensorGraph::FlagsAll & ~TensorGraph::FlagFormatRf32)) return false;
			
			// load model
			if(!tensor_onnx.load(device, DATA_PATH "model.onnx", TensorGraph::FlagFormatRf16)) return false;
			
			// create input tensor
			input_buffer = device.createBuffer(Buffer::FlagStorage, sizeof(float16_t) * size * size * 3);
			input_tensor = Tensor(&input_buffer, FormatRf16, size, size, 3);
			if(!input_buffer) return false;
			
			// create output buffer
			output_buffer = device.createBuffer(Buffer::FlagStorage | Buffer::FlagSource, sizeof(float16_t) * 1000);
			if(!output_buffer) return false;
			
			// create temporary buffer
			tensor_buffer = device.createBuffer(Buffer::FlagStorage, 1024 * 1024 * 8);
			if(!tensor_buffer) return false;
			
			return true;
		}
		
		// main loop
		bool main() {
			
			// begin preview
			if(!capture.beginPreview(luma_texture, chroma_texture)) return false;
			
			// main loop
			window.run(makeClassFunction(this, &Detector::render));
			
			// end preview
			if(!capture.endPreview()) return false;
			
			// finish context
			window.finish();
			
			return true;
		}
		
	private:
		
		// render app
		bool render() {
			
			// update preview
			bool update_preview = capture.updatePreview(device);
			
			// update window
			Window::update();
			
			// render window
			if(!window.render()) return false;
			
			// flush textures
			device.flushTextures({ luma_texture, chroma_texture });
			
			{
				// create command list
				Compute compute = device.createCompute();
				
				// crop texture
				if(update_preview) {
					compute.setKernel(kernel);
					compute.setSampler(0, sampler);
					compute.setTextures(0, { luma_texture, chroma_texture });
					compute.setSurfaceTexture(0, rgb_texture);
					compute.dispatch(rgb_texture);
					compute.barrier(rgb_texture);
				}
				
				// copy texture to tensor
				tensor_graph.dispatch(compute, input_tensor.setScaleBias(1.0f / 0.229f, -0.485f / 0.229f), rgb_texture);
				
				// dispatch model
				Tensor output_tensor(&output_buffer);
				if(!tensor_onnx.dispatch(tensor_graph, compute, output_tensor, input_tensor, tensor_buffer)) return false;
			}
			
			// finish device
			device.finish();
			
			// print class
			uint32_t indices[4] = {};
			float32_t weights[4] = {};
			Array<float16_t> output(1000);
			device.getBuffer(output_buffer, output.get(), output.bytes());
			for(uint32_t i = 0; i < output.size(); i++) {
				if(weights[0] < output[i].getFast()) {
					for(uint32_t j = TS_COUNTOF(indices) - 1; j > 0; j--) {
						weights[j] = weights[j - 1];
						indices[j] = indices[j - 1];
					}
					weights[0] = output[i].getFast();
					indices[0] = i;
				}
			}
			if(weights[0] > 0.0f) {
				String info;
				for(uint32_t i = 0; i < TS_COUNTOF(indices); i++) {
					info += String::format("\n%s (%.1f)", classes[indices[i]].get(), weights[i]);
				}
				panel->setInfo(info);
			}
			
			// flush texture
			device.flushTexture(rgb_texture);
			
			// texture preview
			float32_t height = 900.0f;
			float32_t width = Tellusim::floor(height * (float32_t)window.getWidth() / (float32_t)window.getHeight());
			float32_t size = min(width, height);
			texture_rect.setTexture(rgb_texture);
			texture_rect.setSize(size, size);
			
			// update panel
			panel->update(window, device, target);
			
			// window target
			target.begin();
			{
				// create command list
				Command command = device.createCommand(target);
				
				// draw panel
				panel->draw(command, target);
			}
			target.end();
			
			// present window
			if(!window.present()) return false;
			
			// check errors
			if(!device.check()) return false;
			
			return true;
		}
		
		App &app;
		
		Window window;
		
		Device device;
		Target target;
		
		Capture capture;
		
		AutoPtr<Panel> panel;
		
		Array<String> classes;
		
		ControlRect texture_rect;
		
		Kernel kernel;
		
		Sampler sampler;
		
		Texture rgb_texture;
		Texture luma_texture;
		Texture chroma_texture;
		
		TensorGraph tensor_graph;
		TensorONNX tensor_onnx;
		
		Buffer input_buffer;
		Buffer output_buffer;
		Buffer tensor_buffer;
		
		Tensor input_tensor;
};

/*
 */
int32_t main(int32_t argc, char **argv) {
	
	// create app
	App app(argc, argv);
	if(!app.create()) return 1;
	
	// create Detector
	Detector detector(app);
	if(!detector.create("tsDetector")) return 1;
	
	// main loop
	if(!detector.main()) return 1;
	
	return 0;
}

/*
 */
#if _WINAPP
	#include <system/TellusimWinApp.h>
	TS_DECLARE_WINAPP_MAIN
#endif
#if _ANDROID
	#include <system/TellusimAndroid.h>
	TS_DECLARE_ANDROID_NATIVE_ACTIVITY
#endif
