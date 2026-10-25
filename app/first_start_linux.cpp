// Linux first start: tools/make_module.sh compiles the game code from the player's disc, which
// takes minutes (more on a Steam Deck). Adapted from the NFL Blitz 2003 port's first_start.cpp. Steam Deck Game Mode and starts from Steam or a file manager
// have no terminal, and the game's window does not exist yet: this window asks first, then shows the
// build's step, a progress bar and the elapsed time, and on failure the end of the output (all of it
// is in build.log). Controller (A / B), keyboard (Enter / Esc) or mouse. SDL's built-in debug font:
// no font files needed.
#include "first_start.h"

#include "kernel/kernel.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace tz::firststart {

namespace {

constexpr float kW = 640, kH = 360;  // layout units; 8x8 debug font -> 80 columns
constexpr int kCols = 76;

// The build's output as the window sees it.
struct Progress {
  std::mutex m;
  std::string step = "Starting";
  std::string last;
  std::deque<std::string> tail;  // last lines, for a failure
  int total = 0, compiled = 0;
  bool eof = false;
};

std::vector<std::string> wrap(const std::string& text, size_t cols) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    std::string para = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = nl == std::string::npos ? text.size() : nl + 1;
    if (para.empty()) out.emplace_back();
    while (!para.empty()) {
      size_t cut = para.size() <= cols ? para.size() : para.rfind(' ', cols);
      if (cut == std::string::npos || cut == 0) cut = std::min(cols, para.size());
      out.push_back(para.substr(0, cut));
      para.erase(0, cut);
      while (!para.empty() && para[0] == ' ') para.erase(0, 1);
    }
  }
  return out;
}

class Window {
 public:
  bool open() {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
      logf("[boot] first start: no display (%s)", SDL_GetError());
      return false;
    }
    m_inited = true;
    m_win = SDL_CreateWindow("Tenchu Z Recomp - first start", 1280, 720, SDL_WINDOW_RESIZABLE);
    if (!m_win) {
      logf("[boot] first start: cannot open a window (%s)", SDL_GetError());
      return false;
    }
    // SDL's software renderer: text and a bar need no GPU, and the default (OpenGL) renderer crashed
    // in Debian 12's Mesa 22 while the build ran.
    m_ren = SDL_CreateRenderer(m_win, "software");
    if (!m_ren) m_ren = SDL_CreateRenderer(m_win, nullptr);
    if (!m_ren) {
      logf("[boot] first start: cannot draw in the window (%s)", SDL_GetError());
      return false;
    }
    SDL_SetRenderVSync(m_ren, 1);
    return true;
  }
  ~Window() {
    for (SDL_Gamepad* g : m_pads) SDL_CloseGamepad(g);
    if (m_ren) SDL_DestroyRenderer(m_ren);
    if (m_win) SDL_DestroyWindow(m_win);
    if (m_inited) SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
  }

  enum class Input { None, Yes, No, Quit };
  // Events since the last call: A / Enter / a click on the left button = Yes, B / Esc / the right
  // button = No, closing the window (or SIGTERM from Steam) = Quit.
  Input poll() {
    Input r = Input::None;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      switch (ev.type) {
      case SDL_EVENT_QUIT:
      case SDL_EVENT_WINDOW_CLOSE_REQUESTED: return Input::Quit;
      case SDL_EVENT_GAMEPAD_ADDED:
        if (SDL_Gamepad* g = SDL_OpenGamepad(ev.gdevice.which)) m_pads.push_back(g);
        break;
      case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH || ev.gbutton.button == SDL_GAMEPAD_BUTTON_START) r = Input::Yes;
        if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_EAST || ev.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) r = Input::No;
        break;
      case SDL_EVENT_KEY_DOWN:
        if (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER || ev.key.key == SDLK_SPACE) r = Input::Yes;
        if (ev.key.key == SDLK_ESCAPE) r = Input::No;
        break;
      case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        SDL_ConvertEventToRenderCoordinates(m_ren, &ev);
        float x = ev.button.x - m_ox, y = ev.button.y - m_oy;
        for (int i = 0; i < 2; i++)
          if (m_buttons[i] && x >= m_btn[i].x && x < m_btn[i].x + m_btn[i].w && y >= m_btn[i].y && y < m_btn[i].y + m_btn[i].h)
            r = i == 0 ? Input::Yes : Input::No;
        break;
      }
      default: break;
      }
    }
    return r;
  }

  // One frame: a title, text lines, a progress bar (fraction 0..1; < 0: none; > 1: busy animation)
  // and up to two buttons.
  void draw(const std::vector<std::string>& lines, float progress, const char* yes, const char* no) {
    int pw = 0, ph = 0;
    SDL_GetCurrentRenderOutputSize(m_ren, &pw, &ph);
    float s = std::max(0.5f, std::min(pw / kW, ph / kH));
    SDL_SetRenderScale(m_ren, s, s);
    m_ox = (pw / s - kW) / 2;
    m_oy = (ph / s - kH) / 2;
    SDL_SetRenderDrawColor(m_ren, 10, 18, 40, 255);
    SDL_RenderClear(m_ren);
    SDL_SetRenderDrawColor(m_ren, 200, 30, 40, 255);
    SDL_FRect bar{m_ox, m_oy + 20, kW, 2};
    SDL_RenderFillRect(m_ren, &bar);
    SDL_SetRenderDrawColor(m_ren, 255, 255, 255, 255);
    SDL_RenderDebugText(m_ren, m_ox + 12, m_oy + 8, "TENCHU Z RECOMP - FIRST START");
    float y = m_oy + 36;
    for (const std::string& l : lines) {
      SDL_RenderDebugText(m_ren, m_ox + 12, y, l.c_str());
      y += 11;
    }
    if (progress >= 0) {
      SDL_FRect frame{m_ox + 12, m_oy + 276, kW - 24, 14};
      SDL_SetRenderDrawColor(m_ren, 120, 140, 180, 255);
      SDL_RenderRect(m_ren, &frame);
      SDL_SetRenderDrawColor(m_ren, 60, 140, 255, 255);
      SDL_FRect fill{frame.x + 2, frame.y + 2, 0, frame.h - 4};
      if (progress <= 1) {
        fill.w = (frame.w - 4) * progress;
      } else {  // busy: a block moving back and forth
        float t = (float)(SDL_GetTicks() % 2000) / 1000.0f;
        float p = t < 1 ? t : 2 - t;
        fill.w = (frame.w - 4) * 0.2f;
        fill.x += (frame.w - 4 - fill.w) * p;
      }
      SDL_RenderFillRect(m_ren, &fill);
    }
    const char* labels[2] = {yes, no};
    for (int i = 0; i < 2; i++) {
      m_buttons[i] = labels[i] != nullptr;
      if (!labels[i]) continue;
      m_btn[i] = {i == 0 ? 120.0f : 340.0f, 318, 180, 22};
      SDL_FRect b{m_ox + m_btn[i].x, m_oy + m_btn[i].y, m_btn[i].w, m_btn[i].h};
      SDL_SetRenderDrawColor(m_ren, i == 0 ? 40 : 70, i == 0 ? 110 : 40, i == 0 ? 60 : 50, 255);
      SDL_RenderFillRect(m_ren, &b);
      SDL_SetRenderDrawColor(m_ren, 255, 255, 255, 255);
      float tx = b.x + (b.w - 8.0f * (float)strlen(labels[i])) / 2;
      SDL_RenderDebugText(m_ren, tx, b.y + 7, labels[i]);
    }
    SDL_RenderPresent(m_ren);
  }

 private:
  bool m_inited = false;
  SDL_Window* m_win = nullptr;
  SDL_Renderer* m_ren = nullptr;
  std::vector<SDL_Gamepad*> m_pads;
  float m_ox = 0, m_oy = 0;
  SDL_FRect m_btn[2] = {};
  bool m_buttons[2] = {};
};

// Shows `text` with one button until it is pressed or the window closes.
void message(Window* w, const std::string& text) {
  std::vector<std::string> lines = wrap(text, kCols);
  while (w) {
    w->draw(lines, -1, "A / Enter: OK", nullptr);
    Window::Input in = w->poll();
    if (in != Window::Input::None) return;
    SDL_Delay(16);
  }
}

}  // namespace

bool build(const std::string& script, const std::string& xexPath, const std::string& outPath, bool ask) {
  namespace fs = std::filesystem;
  const std::string work = fs::path(xexPath).parent_path().string();
  const std::string logPath = work + "/build.log";
  Window win;
  Window* ui = win.open() ? &win : nullptr;

  // One build at a time: a second start during the build would delete the first one's compiler.
  int lock = open((work + "/lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
  if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) != 0) {
    logf("[boot] first start: another Tenchu Z is compiling the game code");
    message(ui, "Another copy of Tenchu Z is compiling the game code right now.\n\n"
                "Wait for it to finish, then start the game again.");
    close(lock);
    return false;
  }

  if (ask) {
    if (!ui) {
      logf("[boot] the game code is not compiled yet and no window can be shown to ask: start the game with "
           "TENCHU_AUTO_BUILD=1 to compile it now");
      if (lock >= 0) close(lock);
      return false;
    }
    std::vector<std::string> lines = wrap(
        "Before the first game, Tenchu Z compiles the game code from your disc. This happens once and "
        "takes several minutes (on a Steam Deck about fifteen).\n\n"
        "Everything it needs comes with the game, apart from python3, which SteamOS and desktop Linux "
        "include.",
        kCols);
    for (;;) {
      ui->draw(lines, -1, "A / Enter: Compile", "B / Esc: Quit");
      Window::Input in = ui->poll();
      if (in == Window::Input::Yes) break;
      if (in == Window::Input::No || in == Window::Input::Quit) {
        if (lock >= 0) close(lock);
        return false;
      }
      SDL_Delay(16);
    }
  }

  // The build: its own process group (cancelled as a whole), stdout+stderr into a pipe, without
  // Steam's LD_PRELOAD (its overlay libraries fill the output with loader errors in every tool).
  int fds[2];
  if (pipe(fds) != 0) {
    message(ui, "Cannot start the compiler (pipe).");
    if (lock >= 0) close(lock);
    return false;
  }
  // Arguments and environment made before fork: only async-signal-safe calls between fork and exec.
  std::vector<char*> env;
  for (char** e = environ; *e; e++)
    if (strncmp(*e, "LD_PRELOAD=", 11) != 0) env.push_back(*e);
  env.push_back(nullptr);
  const char* argv[] = {"sh", script.c_str(), xexPath.c_str(), outPath.c_str(), work.c_str(), nullptr};
  pid_t parent = getpid();
  pid_t pid = fork();
  if (pid == 0) {
    setpgid(0, 0);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() != parent) _exit(127);
    dup2(fds[1], 1);
    dup2(fds[1], 2);
    close(fds[0]);
    close(fds[1]);
    execve("/bin/sh", const_cast<char* const*>(argv), env.data());
    _exit(127);
  }
  if (pid > 0) setpgid(pid, pid);  // also here: the group must exist before a cancel signals it
  close(fds[1]);
  if (pid < 0) {
    close(fds[0]);
    message(ui, "Cannot start the compiler (fork).");
    if (lock >= 0) close(lock);
    return false;
  }
  logf("[boot] compiling the game code: sh %s (log %s)", script.c_str(), logPath.c_str());

  Progress prog;
  std::thread reader([&] {
    FILE* log = fopen(logPath.c_str(), "w");
    std::string line;
    char buf[4096];
    auto take = [&](const std::string& l) {
      if (log) fprintf(log, "%s\n", l.c_str()), fflush(log);
      fprintf(stdout, "%s\n", l.c_str());
      fflush(stdout);
      std::lock_guard lk(prog.m);
      if (l.rfind("# ", 0) == 0) {
        prog.step = l.substr(2);
        int n = 0;
        if (sscanf(l.c_str(), "# Compiling %d files", &n) == 1) prog.total = n;
      } else if (int done = 0, total = 0; sscanf(l.c_str(), "[%d/%d]", &done, &total) == 2) {
        prog.compiled = done;  // compile_module.py: "[n/N] file" per finished unit
      }
      if (!l.empty()) prog.last = l;
      if (l.rfind("xargs: ", 0) == 0) return;  // "...exited with status 255; aborting" once per job
      prog.tail.push_back(l);
      if (prog.tail.size() > 14) prog.tail.pop_front();
    };
    for (ssize_t n; (n = read(fds[0], buf, sizeof(buf))) > 0 || (n < 0 && errno == EINTR);) {
      if (n < 0) continue;
      line.append(buf, (size_t)n);
      for (size_t nl; (nl = line.find('\n')) != std::string::npos; line.erase(0, nl + 1)) take(line.substr(0, nl));
    }
    if (!line.empty()) take(line);
    if (log) fclose(log);
    close(fds[0]);
    std::lock_guard lk(prog.m);
    prog.eof = true;
  });

  const uint64_t t0 = SDL_GetTicks();
  bool cancelled = false;
  for (;;) {
    {
      std::lock_guard lk(prog.m);
      if (prog.eof) break;
    }
    if (ui) {
      std::string step, last;
      float frac = 2;  // busy until the compile step reports its file count
      {
        std::lock_guard lk(prog.m);
        step = prog.step;
        last = prog.last;
        if (prog.total > 0) frac = 0.1f + 0.85f * (float)prog.compiled / (float)prog.total;
        if (step == "Linking") frac = 0.97f;
      }
      uint64_t sec = (SDL_GetTicks() - t0) / 1000;
      char elapsed[64];
      snprintf(elapsed, sizeof(elapsed), "%llu:%02llu", (unsigned long long)(sec / 60), (unsigned long long)(sec % 60));
      std::vector<std::string> lines = wrap(
          "Compiling the game code from your disc. This happens only once; please wait.", kCols);
      lines.emplace_back();
      lines.push_back(step + "...");
      lines.push_back(std::string("Time: ") + elapsed);
      lines.emplace_back();
      if (last.size() > (size_t)kCols) last = last.substr(0, kCols - 3) + "...";
      lines.push_back(last);
      ui->draw(lines, frac, nullptr, nullptr);
      if (ui->poll() == Window::Input::Quit) {
        cancelled = true;
        break;
      }
    }
    SDL_Delay(ui ? 33 : 200);
  }
  int status = 0;
  if (cancelled) {
    // TERM to the build's process group, then KILL if it is still there after 5 s (the window keeps
    // drawing meanwhile); the reader ends when the last process holding the pipe is gone.
    if (kill(-pid, SIGTERM) != 0) kill(pid, SIGTERM);
    const uint64_t until = SDL_GetTicks() + 5000;
    while (SDL_GetTicks() < until) {
      {
        std::lock_guard lk(prog.m);
        if (prog.eof) break;
      }
      if (ui) {
        ui->draw({"Cancelling..."}, 2, nullptr, nullptr);
        ui->poll();
      }
      SDL_Delay(33);
    }
    if (kill(-pid, SIGKILL) != 0) kill(pid, SIGKILL);
  }
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  reader.join();
  if (lock >= 0) close(lock);
  if (cancelled) {
    logf("[boot] first start: compiling cancelled");
    return false;
  }
  bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  if (ok) {
    logf("[boot] first start: game code compiled in %llu s", (unsigned long long)((SDL_GetTicks() - t0) / 1000));
    return true;
  }
  logf("[boot] compiling the game code failed (exit status %d): see %s", WIFEXITED(status) ? WEXITSTATUS(status) : -1,
       logPath.c_str());
  std::string text = "Compiling the game code failed. The end of its output:\n\n";
  for (const std::string& l : prog.tail) text += (l.size() > (size_t)kCols ? l.substr(0, kCols - 3) + "..." : l) + "\n";
  text += "\nAll of it is in " + logPath;
  message(ui, text);
  return false;
}

}  // namespace tz::firststart
