#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <cstdlib> // For getenv

#include "ftxui/component/captured_mouse.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/component_base.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"

using namespace ftxui;
namespace fs = std::filesystem;

enum Screens {
    SCREEN_CONNECTION = 0,
    SCREEN_MAIN = 1,
    SCREEN_FILE_DIALOG = 2
};

int main() {
    auto screen = ScreenInteractive::Fullscreen();
    int active_screen = SCREEN_CONNECTION;

    // --- State Variables ---
    std::string my_code = "Generating secure key...";
    std::string peer_code;
    std::string chat_input;
    bool is_connecting = false;
    std::string connect_btn_label = "Connect";

    // File Explorer State
    std::string current_loaded_path = fs::current_path().string();
    std::string path_input = current_loaded_path;
    std::string search_input;
    std::string previous_search_input = "";

    int selected_file_index = 0;
    int last_confirmed_index = -1;

    std::vector<std::string> chat_history;
    std::vector<std::string> active_transfers;
    std::vector<std::string> all_directory_files;
    std::vector<std::string> current_directory_files;

    // ==========================================
    // BACKEND LOGIC: FILE SYSTEM & SEARCH
    // ==========================================
    auto apply_search_filter = [&]() {
        current_directory_files.clear();

        if (search_input.empty()) {
            current_directory_files = all_directory_files;
        } else {
            std::string search_lower = search_input;
            std::transform(search_lower.begin(), search_lower.end(), search_lower.begin(),
                           [](unsigned char c){ return std::tolower(c); });

            for (const auto& file : all_directory_files) {
                if (file == "..") {
                    current_directory_files.push_back(file);
                    continue;
                }

                std::string file_lower = file;
                std::transform(file_lower.begin(), file_lower.end(), file_lower.begin(),
                               [](unsigned char c){ return std::tolower(c); });

                if (file_lower.find(search_lower) != std::string::npos) {
                    current_directory_files.push_back(file);
                }
            }
        }

        selected_file_index = 0;
        last_confirmed_index = -1;
    };

    auto load_directory = [&]() {
        current_loaded_path = path_input;
        all_directory_files.clear();
        all_directory_files.push_back("..");

        std::vector<std::string> directories;
        std::vector<std::string> files;

        try {
            for (const auto& entry : fs::directory_iterator(path_input)) {
                std::string filename = entry.path().filename().string();
                if (entry.is_directory()) {
                    directories.push_back(filename + "/");
                } else {
                    files.push_back(filename);
                }
            }

            std::sort(directories.begin(), directories.end());
            std::sort(files.begin(), files.end());

            all_directory_files.insert(all_directory_files.end(), directories.begin(), directories.end());
            all_directory_files.insert(all_directory_files.end(), files.begin(), files.end());

        } catch (const fs::filesystem_error& e) {
            all_directory_files.push_back("<Access Denied>");
        }

        search_input.clear();
        previous_search_input.clear();
        apply_search_filter();
    };

    // Initial load
    load_directory();

    // ==========================================
    // BULLETPROOF BUTTON RENDERING
    // ==========================================
    ButtonOption safe_button;
    safe_button.transform = [](const EntryState& s) {
        auto content = text(s.label) | center;
        if (s.focused) {
            content = content | bold | inverted;
        }
        return content | border;
    };

    // ==========================================
    // SCREEN 1: CONNECTION
    // ==========================================
    InputOption peer_input_option;
    peer_input_option.multiline = false;
    peer_input_option.on_enter = [&] {
        if (!peer_code.empty()) {
            is_connecting = true;
            connect_btn_label = "Cancel";
        }
    };

    Component input_peer_code = Input(&peer_code, "Enter peer code...", peer_input_option);
    Component conditional_input = Maybe(input_peer_code, [&] { return !is_connecting; });

    Component btn_connect_cancel = Button(&connect_btn_label, [&] {
        is_connecting = !is_connecting;
        connect_btn_label = is_connecting ? "Cancel" : "Connect";
    }, safe_button);

    Component btn_force_start = Button("Force Start (Simulate Success)", [&] {
        active_screen = SCREEN_MAIN;
    }, safe_button);

    auto connection_layout = Container::Vertical({
        conditional_input,
        btn_connect_cancel,
        btn_force_start
    });

    auto connection_renderer = Renderer(connection_layout, [&] {
        auto status_text = is_connecting
            ? text("Status: Connecting... Waiting for hole punch") | color(Color::Yellow)
            : text("Status: Idle") | color(Color::Green);

        Elements bottom_elements;
        bottom_elements.push_back(status_text | hcenter);
        bottom_elements.push_back(text("") | size(HEIGHT, EQUAL, 1));

        if (!is_connecting) {
            bottom_elements.push_back(input_peer_code->Render() | size(WIDTH, EQUAL, 35) | hcenter);
        } else {
            bottom_elements.push_back(text("") | size(HEIGHT, EQUAL, 3));
        }

        bottom_elements.push_back(text("") | size(HEIGHT, EQUAL, 1));
        bottom_elements.push_back(btn_connect_cancel->Render() | size(WIDTH, EQUAL, 20) | hcenter);

        return window(text(" P2P Setup "),
            vbox({
                filler(),
                vbox({
                    text("Share this secure code with your peer:") | hcenter,
                    text(my_code) | bold | hcenter | color(Color::Cyan),
                }),
                filler(),
                separator(),
                vbox(bottom_elements) | size(HEIGHT, EQUAL, 10),
                btn_force_start->Render() | size(WIDTH, EQUAL, 35) | hcenter
            })
        ) | flex;
    });

    // ==========================================
    // SCREEN 2: MAIN DASHBOARD
    // ==========================================
    InputOption chat_input_option;
    chat_input_option.multiline = false;
    chat_input_option.on_enter = [&] {
        if (!chat_input.empty()) {
            chat_history.push_back("You: " + chat_input);
            chat_input.clear();
        }
    };
    Component input_chat = Input(&chat_input, "Type message...", chat_input_option);

    Component btn_browse = Button("Browse Files & Folders", [&] {
        active_screen = SCREEN_FILE_DIALOG;
    }, safe_button);

    Component btn_exit = Button("Exit Application", screen.ExitLoopClosure(), safe_button);

    auto chat_container = Container::Vertical({ input_chat });
    auto sidebar_actions = Container::Vertical({ btn_browse, btn_exit });

    auto main_screen_layout = Container::Horizontal({
        chat_container,
        sidebar_actions
    });

    auto chat_history_renderer = Renderer([&] {
        Elements elements;
        for (const auto& msg : chat_history) {
            elements.push_back(paragraph(msg));
        }
        if (elements.empty()) elements.push_back(text("No messages yet...") | dim);
        return vbox(std::move(elements)) | focusPositionRelative(0, 1) | yframe | flex;
    });

    auto transfers_renderer = Renderer([&] {
        Elements elements;
        for (const auto& transfer : active_transfers) {
            elements.push_back(text(transfer));
        }
        if (elements.empty()) elements.push_back(text("No active transfers") | dim);
        return vbox(std::move(elements)) | yframe | flex;
    });

    auto main_screen_renderer = Renderer(main_screen_layout, [&] {
        auto chat_panel = vbox({
            window(text(" Conversation "), chat_history_renderer->Render()),
            window(text(" Input "), input_chat->Render())
        }) | flex;

        auto sidebar_panel = vbox({
            window(text(" Transfers "), transfers_renderer->Render()),
            window(text(" Actions "),
                vbox({
                    btn_browse->Render(),
                    filler(),
                    btn_exit->Render()
                })
            ) | size(HEIGHT, EQUAL, 9)
        });

        return window(text(" P2P Dashboard "),
            hbox({
                chat_panel,
                separator(),
                sidebar_panel | size(WIDTH, EQUAL, 35)
            })
        ) | flex;
    });

    // ==========================================
    // SCREEN 3: FILE EXPLORER
    // ==========================================

    // THE FIX: Robust Path Resolver
    InputOption path_input_option;
    path_input_option.multiline = false;
    path_input_option.on_enter = [&] {
        std::string target = path_input;

        // 1. Expand "~" to the user's home directory natively
        if (!target.empty() && target[0] == '~') {
            const char* home = getenv("HOME");
            if (home) {
                target.replace(0, 1, std::string(home));
            }
        }

        // 2. Resolve relative paths against the currently loaded path
        fs::path p(target);
        if (!p.is_absolute()) {
            p = fs::path(current_loaded_path) / p;
            target = p.string();
        }

        // 3. Strip trailing slashes so validation always passes (unless root "/")
        while (target.length() > 1 && target.back() == '/') {
            target.pop_back();
        }

        std::error_code ec;
        if (fs::exists(target, ec) && fs::is_directory(target, ec)) {
            // 4. fs::canonical resolves internal '..' and gives a pure absolute string
            path_input = fs::canonical(target, ec).string();
            if (ec) path_input = target; // Fallback if canonical fails

            load_directory();
        } else {
            // If it truly fails, revert gracefully
            path_input = current_loaded_path;
        }
    };

    Component input_path = Input(&path_input, "Enter path manually...", path_input_option);

    InputOption search_input_option;
    search_input_option.multiline = false;
    search_input_option.on_change = [&] {
        if (search_input != previous_search_input) {
            previous_search_input = search_input;
            apply_search_filter();
        }
    };
    Component input_search = Input(&search_input, "Search files...", search_input_option);

    MenuOption file_menu_option;
    file_menu_option.on_enter = [&] {
        if (current_directory_files.empty() || current_directory_files[0] == "<Access Denied>") return;

        if (selected_file_index == last_confirmed_index) {
            std::string selected_item = current_directory_files[selected_file_index];

            if (selected_item == "..") {
                path_input = fs::path(path_input).parent_path().string();
                load_directory();
            } else if (selected_item.back() == '/') {
                selected_item.pop_back();
                path_input = (fs::path(path_input) / selected_item).string();
                load_directory();
            } else {
                active_transfers.push_back("Queued: " + selected_item);
                last_confirmed_index = -1;
                active_screen = SCREEN_MAIN;
            }
        } else {
            last_confirmed_index = selected_file_index;
        }
    };

    Component file_menu = Menu(&current_directory_files, &selected_file_index, file_menu_option);

    Component btn_cancel = Button("Cancel", [&] { active_screen = SCREEN_MAIN; }, safe_button);
    Component btn_confirm = Button("Confirm", [&] {
        if (last_confirmed_index != -1 && last_confirmed_index < current_directory_files.size()) {
            std::string selected_item = current_directory_files[last_confirmed_index];
            if (selected_item.back() == '/') selected_item.pop_back();

            active_transfers.push_back("Queued: " + selected_item);
        }
        active_screen = SCREEN_MAIN;
    }, safe_button);

    auto file_dialog_layout = Container::Vertical({
        input_path,
        input_search,
        file_menu,
        Container::Horizontal({btn_cancel, btn_confirm})
    });

    auto file_dialog_renderer = Renderer(file_dialog_layout, [&] {
        std::string selection_text = " Selected: ";
        if (last_confirmed_index != -1 && last_confirmed_index < current_directory_files.size()) {
            std::string selected_item = current_directory_files[last_confirmed_index];
            if (selected_item.back() == '/') selected_item.pop_back();
            selection_text += (fs::path(path_input) / selected_item).string();
        } else {
            selection_text += "None";
        }

        return window(text(" File Explorer "),
            vbox({
                hbox({text(" Path (Press Enter to jump): "), input_path->Render() | flex}),
                separator(),
                hbox({text(" Search: "), input_search->Render() | flex}),
                separator(),
                file_menu->Render() | vscroll_indicator | frame | flex,
                separator(),
                hbox({
                    text(selection_text) | color(Color::Cyan) | flex,
                    btn_cancel->Render(),
                    btn_confirm->Render()
                })
            })
        ) | flex;
    });

    // ==========================================
    // MASTER CONTROLLER
    // ==========================================
    auto master_tabs = Container::Tab({
        connection_renderer,
        main_screen_renderer,
        file_dialog_renderer
    }, &active_screen);

    auto global_renderer = CatchEvent(master_tabs, [&](Event event) {
        if (event == Event::Escape || event == Event::Character('q')) {
            screen.ExitLoopClosure()();
            return true;
        }
        return false;
    });

    screen.Loop(global_renderer);
    return 0;
}