#pragma once

#include "../window.h"

#if defined(__ANDROID__) && defined(IMPACTO_SHINKI_EMU)
#include <EGL/egl.h>
#endif

namespace Impacto {
namespace OpenGL {

class GLWindow : public BaseWindow {
 public:
  void Init() override;
  void SetDimensions(int width, int height, int msaa,
                     float renderScale) override;
  void SwapRTs() override;
  void Update() override;
  void Draw() override;
  void Shutdown() override;

#if !defined(__ANDROID__) || !defined(IMPACTO_SHINKI_EMU)
  SDL_GLContext GLContext;
#endif

  // FBO of current render target for drawing onto
  GLuint DrawRT;
  // FBO of current render target for reading from
  GLuint ReadRT;
  // Texture associated with ReadRT
  GLuint ReadRenderTexture;
  // Stencil buffer associated with both ReadRT and DrawRT
  GLuint StencilBuffer;

 private:
  void UpdateDimensions() override;
#if defined(__ANDROID__) && defined(IMPACTO_SHINKI_EMU)
  void CreateAndroidGL();
  EGLDisplay Display = EGL_NO_DISPLAY;
  EGLSurface Pbuffer = EGL_NO_SURFACE;
  EGLContext Context = EGL_NO_CONTEXT;
  bool GLReady = false;
#else
  void TryCreateGL(GraphicsApi api);
#endif
  void CleanFBOs();

  [[maybe_unused]] bool GLDebug = false;
  GLuint DrawRenderTexture = 0;
};

}  // namespace OpenGL
}  // namespace Impacto