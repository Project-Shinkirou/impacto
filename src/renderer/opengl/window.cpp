#include "window.h"

#include <glad/glad.h>
#include <SDL3/SDL_opengl.h>
#if defined(__ANDROID__) && defined(IMPACTO_SHINKI_EMU)
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>
#include <stdexcept>
#endif

#include "../renderer.h"

#include "../../log.h"
#include "glc.h"

#include "../../profile/game.h"
#include "../../profile/profile.h"
#include "../../userconfig.h"

#ifndef IMPACTO_DISABLE_IMGUI
#include <imgui_custom/backends/imgui_impl_opengl3.h>
#endif
#include "../../game.h"
#include "../../profile/vm.h"

namespace Impacto {
namespace OpenGL {

void GLWindow::UpdateDimensions() {
  WindowDimensionsChanged = false;
  SDL_GetWindowSizeInPixels(SDLWindow, &WindowWidth, &WindowHeight);
  if (WindowWidth != lastWidth || WindowHeight != lastHeight ||
      MsaaCount != lastMsaa || RenderScale != lastRenderScale) {
    WindowDimensionsChanged = true;
    ImpLog(LogLevel::Debug, LogChannel::General,
           "Drawable size (pixels): {:d} x {:d} ({:d}x MSAA requested, render "
           "scale "
           "{:f})\n",
           WindowWidth, WindowHeight, MsaaCount, RenderScale);
  }
  lastWidth = WindowWidth;
  lastHeight = WindowHeight;
  lastMsaa = MsaaCount;
  lastRenderScale = RenderScale;
  DpiScale = SDL_GetWindowDisplayScale(SDLWindow);

  // SDL_SetWindowInputFocus(SDLWindow);
}

#if defined(__ANDROID__) && defined(IMPACTO_SHINKI_EMU)
void GLWindow::CreateAndroidGL() {
  // The Manager owns the Activity and physical surface. SDL provides an
  // in-memory window and event queue; EGL owns this worker's native raster.
  SDL_SetMainReady();
  if (!SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "offscreen", SDL_HINT_OVERRIDE) ||
      !SDL_Init(SDL_INIT_VIDEO) || !CreateSDLWindow(0))
    throw std::runtime_error(SDL_GetError());

  const auto check = [](bool success) {
    if (!success)
      throw std::runtime_error(fmt::format("Impacto EGL error 0x{:x}", eglGetError()));
  };
  EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  check(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr));
  Display = display;
  check(eglBindAPI(EGL_OPENGL_ES_API));
  const EGLint configAttributes[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
      EGL_NONE,
  };
  EGLConfig config;
  EGLint count;
  check(eglChooseConfig(Display, configAttributes, &config, 1, &count) == EGL_TRUE);
  if (count != 1) throw std::runtime_error("Impacto needs an RGBA8 EGL pbuffer");
  const EGLint surfaceAttributes[] = {
      EGL_WIDTH, WindowWidth, EGL_HEIGHT, WindowHeight, EGL_NONE,
  };
  Pbuffer = eglCreatePbufferSurface(Display, config, surfaceAttributes);
  check(Pbuffer != EGL_NO_SURFACE);
  const EGLint contextAttributes[] = {
      EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2,
#if IMPACTO_GL_DEBUG
      EGL_CONTEXT_OPENGL_DEBUG, EGL_TRUE,
#endif
      EGL_NONE,
  };
  Context = eglCreateContext(Display, config, EGL_NO_CONTEXT, contextAttributes);
  check(Context != EGL_NO_CONTEXT);
  check(eglMakeCurrent(Display, Pbuffer, Pbuffer, Context));
  ActualGraphicsApi = GfxApi_ForceNativeGLES;
}
#else
void GLWindow::TryCreateGL(GraphicsApi api) {
  if (GLContext != NULL) return;

  if (SDLWindow) {
    SDL_DestroyWindow(SDLWindow);
    SDLWindow = NULL;
  }

  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  // Required for blending to work right in Firefox
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
  SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);

  int contextFlags = 0;

  switch (api) {
    case GfxApi_GL:
      ImpLog(LogLevel::Info, LogChannel::General,
             "Trying to create desktop GL context\n");
      SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "0");
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                          SDL_GL_CONTEXT_PROFILE_CORE);

      contextFlags |= SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG;
      break;

    case GfxApi_ForceDesktopGLES:
      ImpLog(LogLevel::Info, LogChannel::General,
             "Trying to create GLES on desktop GL context\n");
      SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "0");
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                          SDL_GL_CONTEXT_PROFILE_ES);
      break;

    case GfxApi_ForceNativeGLES:
      ImpLog(LogLevel::Info, LogChannel::General,
             "Trying to create native GLES context\n");

#ifndef EMSCRIPTEN
      // Emscripten's EGL implementation fails when you ask it for GLES 3.2,
      // though that is what it gives you when building for WebGL 2...
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
      SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
#endif
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                          SDL_GL_CONTEXT_PROFILE_ES);
      break;
  }
#if IMPACTO_GL_DEBUG
  contextFlags |= SDL_GL_CONTEXT_DEBUG_FLAG;
#endif
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, contextFlags);

  SDL_WindowFlags windowFlags = SDL_WINDOW_OPENGL;
  if (!CreateSDLWindow(windowFlags)) return;

  GLContext = SDL_GL_CreateContext(SDLWindow);
  if (GLContext == NULL) {
    ImpLog(LogLevel::Error, LogChannel::General,
           "OpenGL context creation failed: {:s}\n", SDL_GetError());
    return;
  } else {
    ActualGraphicsApi = api;
  }

#ifndef IMPACTO_DISABLE_IMGUI
  ImGui_ImplSDL3_InitForOpenGL(SDLWindow, GLContext);
  ImGui_ImplOpenGL3_Init();
#endif
}
#endif

void GLWindow::Init() {
  assert(IsInit == false);
  ImpLog(LogLevel::Info, LogChannel::General, "Creating window\n");
  IsInit = true;

#if defined(__ANDROID__) && defined(IMPACTO_SHINKI_EMU)
  CreateAndroidGL();
#else
#ifdef __ANDROID__
  SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    ImpLog(LogLevel::Fatal, LogChannel::General,
           "SDL initialisation failed: {:s}\n", SDL_GetError());
    Shutdown();
    return;
  }

  switch (GraphicsApiHint) {
    case GfxApi_GL:
      TryCreateGL(GfxApi_GL);
      TryCreateGL(GfxApi_ForceDesktopGLES);
      TryCreateGL(GfxApi_ForceNativeGLES);
      break;
    case GfxApi_ForceDesktopGLES:
      TryCreateGL(GfxApi_ForceDesktopGLES);
      TryCreateGL(GfxApi_ForceNativeGLES);
      TryCreateGL(GfxApi_GL);
      break;
    case GfxApi_ForceNativeGLES:
      TryCreateGL(GfxApi_ForceNativeGLES);
      TryCreateGL(GfxApi_ForceDesktopGLES);
      TryCreateGL(GfxApi_GL);
      break;
  }

  if (GLContext == NULL) {
    ImpLog(LogLevel::Fatal, LogChannel::General,
           "All options for OpenGL context creation failed\n");
    Shutdown();
    return;
  }
#endif

  SDL_GetWindowSize(SDLWindow, &WindowWidth, &WindowHeight);
  ImpLog(LogLevel::Debug, LogChannel::General,
         "Window size (screen coords): {:d} x {:d}\n", WindowWidth,
         WindowHeight);

#if defined(__ANDROID__) && defined(IMPACTO_SHINKI_EMU)
  bool gladOk = gladLoadGLES2Loader(reinterpret_cast<GLADloadproc>(eglGetProcAddress));
  GLReady = gladOk;
#else
  bool gladOk;
  if (ActualGraphicsApi == GfxApi_GL) {
    gladOk =
        gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress));
  } else {
    gladOk = gladLoadGLES2Loader(
        reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress));
  }
#endif
  if (!gladOk) {
    ImpLog(LogLevel::Fatal, LogChannel::General,
           "GLAD initialisation failed\n");
    Shutdown();
  }

  // At the time of writing, this is on by default on Intel on Windows until we
  // resize the window???
  glDisable(GL_FRAMEBUFFER_SRGB);

#if IMPACTO_GL_DEBUG
  int glContextVersion[2]{};
  glGetIntegerv(GL_MAJOR_VERSION, glContextVersion);
  glGetIntegerv(GL_MINOR_VERSION, glContextVersion + 1);
  bool isDesktopGL = ActualGraphicsApi == GfxApi_GL;
  bool isVer3_2 = glContextVersion[0] >= 3 && glContextVersion[1] >= 2;
  if (!isDesktopGL && !isVer3_2) {
    ImpLog(LogLevel::Error, LogChannel::GL,
           "IMPACTO_GL_DEBUG defined but no debug context requested because "
           "we're using GLES <3.2\n");
  } else {
    GLint flags;
    glGetIntegerv(GL_CONTEXT_FLAGS, &flags);
    if (flags & GL_CONTEXT_FLAG_DEBUG_BIT) {
      GLDebug = true;
      glEnable(GL_DEBUG_OUTPUT);
      glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
      if (isDesktopGL) {
        glDebugMessageCallbackARB(&LogGLMessageCallback, NULL);
        glDebugMessageControlARB(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0,
                                 NULL, GL_TRUE);
      } else if (isVer3_2) {
        glDebugMessageCallback(&LogGLMessageCallback, NULL);
        glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL,
                              GL_TRUE);
      }
    } else {
      ImpLog(LogLevel::Error, LogChannel::GL, "Could not get debug context\n");
    }
  }
#endif

  // Vsync
#if !defined(__ANDROID__) || !defined(IMPACTO_SHINKI_EMU)
  SDL_GL_SetSwapInterval(0);
#endif
}

void GLWindow::SetDimensions(int width, int height, int msaa,
                             float renderScale) {
  ImpLog(LogLevel::Info, LogChannel::General,
         "Attempting to change window dimensions to {:d} x {:d}, {:d}x MSAA, "
         "render "
         "scale {:f}\n",
         width, height, msaa, renderScale);
  assert(width > 0 && height > 0 && msaa >= 0 && renderScale > 0.0f);

  SDL_SetWindowSize(SDLWindow, width, height);

  MsaaCount = msaa;
  RenderScale = renderScale;
}

void GLWindow::CleanFBOs() {
  if (DrawRenderTexture) glDeleteTextures(1, &DrawRenderTexture);
  if (ReadRenderTexture) glDeleteTextures(1, &ReadRenderTexture);
  if (DrawRT) GLC::DeleteFramebuffers(1, &DrawRT);
  if (ReadRT) GLC::DeleteFramebuffers(1, &ReadRT);
  if (StencilBuffer) glDeleteRenderbuffers(1, &StencilBuffer);
  glDeleteTextures((GLsizei)GLC::FramebufferTextures.size(),
                   GLC::FramebufferTextures.data());
  GLC::FramebufferTextures.fill(0);
  GLC::DeleteFramebuffers((GLsizei)GLC::Framebuffers.size(),
                          GLC::Framebuffers.data());
  GLC::Framebuffers.fill(0);
  glDeleteRenderbuffers((GLsizei)GLC::StencilBuffers.size(),
                        GLC::StencilBuffers.data());
  GLC::StencilBuffers.fill(0);
  DrawRenderTexture = ReadRenderTexture = DrawRT = ReadRT = 0;
}

void GLWindow::SwapRTs() {
  std::swap(DrawRT, ReadRT);
  std::swap(DrawRenderTexture, ReadRenderTexture);

  GLC::BindFramebuffer(GL_DRAW_FRAMEBUFFER, DrawRT);
  GLC::BindFramebuffer(GL_READ_FRAMEBUFFER, ReadRT);
}

void GLWindow::Update() {
  UpdateDimensions();

  Rect viewport = GetViewport();

  if (WindowDimensionsChanged || RecreateFBOs) {
    RecreateFBOs = false;
    CleanFBOs();

    auto createFBO = [&](GLuint& frameBufId, GLuint& texture, int bufferType) {
      glGenFramebuffers(1, &frameBufId);
      GLC::BindFramebuffer(bufferType, frameBufId);
      glGenTextures(1, &texture);

      glBindTexture(GL_TEXTURE_2D, texture);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, viewport.Width, viewport.Height,
                   0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

      glFramebufferTexture2D(bufferType, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                             texture, 0);
      glFramebufferRenderbuffer(bufferType, GL_STENCIL_ATTACHMENT,
                                GL_RENDERBUFFER, StencilBuffer);
    };

    glGenRenderbuffers(1, &StencilBuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, StencilBuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, viewport.Width,
                          viewport.Height);
    createFBO(ReadRT, ReadRenderTexture, GL_READ_FRAMEBUFFER);
    createFBO(DrawRT, DrawRenderTexture, GL_DRAW_FRAMEBUFFER);
    GLC::InitializeFramebuffers();

    GLC::BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    for (int i = 0; i < 2; i++) {
      glViewport(0, 0, WindowWidth, WindowHeight);
      glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
#ifndef IMPACTO_SHINKI_EMU
      SDL_GL_SwapWindow(SDLWindow);
#endif
    }
  }

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

  if (viewport.X != 0 || viewport.Y != 0) {
    // Clear outside letter-/pillarbox
    // Unfortunately we seem to need to do this every frame - I get content
    // flickering into the margins on Linux otherwise
    GLC::BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, WindowWidth, WindowHeight);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
  }

  SwapRTs();

  glViewport(0, 0, viewport.Width, viewport.Height);
  glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

  glEnable(GL_BLEND);

  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
}

void GLWindow::Draw() {
  GLC::BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  GLC::BindFramebuffer(GL_READ_FRAMEBUFFER, DrawRT);

  Rect viewport = GetViewport();

  glBlitFramebuffer(0, 0, viewport.Width, viewport.Height, viewport.X,
                    viewport.Y, viewport.Width + viewport.X,
                    viewport.Height + viewport.Y, GL_COLOR_BUFFER_BIT,
                    GL_NEAREST);

#ifndef IMPACTO_DISABLE_IMGUI
  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    ImGui::UpdatePlatformWindows();
    ImGui::RenderPlatformWindowsDefault();
    SDL_GL_MakeCurrent(SDLWindow, GLContext);
  }
#endif

#ifdef IMPACTO_SHINKI_EMU
  Shinki::CaptureFrame(WindowWidth, WindowHeight);
#else
  SDL_GL_SwapWindow(SDLWindow);
#endif
}

void GLWindow::Shutdown() {
#ifdef IMPACTO_SHINKI_EMU
  if (!Shinki::IsClosing()) Shinki::NativeExit(1);
#endif
#if defined(__ANDROID__) && defined(IMPACTO_SHINKI_EMU)
  if (GLReady) CleanFBOs();
  GLReady = false;
  if (Display != EGL_NO_DISPLAY) {
    if (eglGetCurrentContext() == Context && Context != EGL_NO_CONTEXT)
      eglMakeCurrent(Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (Context != EGL_NO_CONTEXT) eglDestroyContext(Display, Context);
    if (Pbuffer != EGL_NO_SURFACE) eglDestroySurface(Display, Pbuffer);
    eglTerminate(Display);
    Context = EGL_NO_CONTEXT;
    Pbuffer = EGL_NO_SURFACE;
    Display = EGL_NO_DISPLAY;
  }
  eglReleaseThread();
#else
  if (GLContext) {
    CleanFBOs();
    SDL_GL_DestroyContext(GLContext);
    GLContext = nullptr;
  }
#endif
  if (SDLWindow) SDL_DestroyWindow(SDLWindow);
  SDLWindow = nullptr;
  IsInit = false;
  SDL_Quit();
#ifndef IMPACTO_SHINKI_EMU
  exit(0);
#endif
}

}  // namespace OpenGL
}  // namespace Impacto