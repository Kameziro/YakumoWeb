// SDL's file dialogs for the web port. Emscripten's SDL3 port has none, and a
// page cannot hand a path on the player's disk to the program anyway: files
// reach it through the page instead (web/). Until that exists, every dialog
// reports itself unavailable, which the setup screens and the file browser
// already handle.

#include <SDL3/SDL.h>

extern "C" {

void SDL_ShowOpenFileDialog(SDL_DialogFileCallback callback, void *userdata, SDL_Window *, const SDL_DialogFileFilter *,
                            int, const char *, bool) {
    SDL_SetError("File dialogs are not available in the browser");
    if (callback != nullptr) callback(userdata, nullptr, -1);
}

void SDL_ShowOpenFolderDialog(SDL_DialogFileCallback callback, void *userdata, SDL_Window *, const char *, bool) {
    SDL_SetError("Folder dialogs are not available in the browser");
    if (callback != nullptr) callback(userdata, nullptr, -1);
}

} // extern "C"
