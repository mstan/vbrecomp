#ifndef VB_SDL_WINDOW_H
#define VB_SDL_WINDOW_H
#include <SDL.h>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <SDL_syswm.h>
#endif

/* The show hint prevents initial activation. The Win32 style also prevents
 * foreground succession when another application's foreground window closes. */
static inline bool vb_sdl_noactivate_window_requested() {
#if defined(_WIN32)
    const char* hint=SDL_GetHint("SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN");
    return hint && SDL_strcmp(hint,"1")==0;
#else
    return false;
#endif
}
static inline bool vb_sdl_prepare_noactivate_window(SDL_Window* window) {
#if defined(_WIN32)
    if(vb_sdl_noactivate_window_requested()) {
        auto fail=[](const char* reason,DWORD error)->bool {
            SDL_SetError("unattended no-activate window: %s (Win32 %lu)",reason,(unsigned long)error);
            return false;
        };
        SDL_SysWMinfo info{};SDL_VERSION(&info.version);
        if(!SDL_GetWindowWMInfo(window,&info) || info.subsystem!=SDL_SYSWM_WINDOWS || !info.info.win.window)
            return fail("owned HWND unavailable",ERROR_INVALID_WINDOW_HANDLE);
        HWND hwnd=info.info.win.window;
        if(IsWindowVisible(hwnd))return fail("window was shown before final style verification",ERROR_INVALID_DATA);
        SetLastError(0);LONG_PTR style=GetWindowLongPtrW(hwnd,GWL_EXSTYLE);DWORD error=GetLastError();
        if(!style && error)return fail("cannot read extended style",error);
        SetLastError(0);LONG_PTR old=SetWindowLongPtrW(hwnd,GWL_EXSTYLE,style|WS_EX_NOACTIVATE);error=GetLastError();
        if(!old && error)return fail("cannot set WS_EX_NOACTIVATE",error);
        if(!SetWindowPos(hwnd,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED))
            return fail("cannot apply extended style",GetLastError());
        if(!(GetWindowLongPtrW(hwnd,GWL_EXSTYLE)&WS_EX_NOACTIVATE))
            return fail("WS_EX_NOACTIVATE verification failed",ERROR_INVALID_DATA);
    }
#else
    (void)window;
#endif
    return true;
}
static inline SDL_Window* vb_sdl_create_window(const char* title,int x,int y,int width,int height,Uint32 flags) {
    const bool guarded=vb_sdl_noactivate_window_requested();
    SDL_Window* window=SDL_CreateWindow(title,x,y,width,height,guarded?((flags&~SDL_WINDOW_SHOWN)|SDL_WINDOW_HIDDEN):flags);
    if(window && guarded && !vb_sdl_prepare_noactivate_window(window)) {
        char error[256];SDL_strlcpy(error,SDL_GetError(),sizeof(error));SDL_DestroyWindow(window);SDL_SetError("%s",error);return nullptr;
    }
    // Guarded callers show only after renderer creation, which can replace HWND.
    return window;
}
static inline bool vb_sdl_show_guarded_window(SDL_Window* window) {
    if(!vb_sdl_prepare_noactivate_window(window))return false;
    SDL_ShowWindow(window);return true;
}
#endif
