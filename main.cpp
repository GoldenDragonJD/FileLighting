#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <cstdlib> // For getenv
#include <map>
#include <cstdint>
#include <fstream>
#include <thread>

#include <asio.hpp>

#include "crypto_utils.hpp"
#include "stun_client.hpp"
#include "ftxui/component/captured_mouse.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/component_base.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"

using namespace ftxui;
using asio::ip::udp;
namespace fs = std::filesystem;

struct FileTransfer {
    uint32_t file_id{};
    std::string file_path;
    size_t total_size{};
    size_t current_size{};
};

struct PeerInformation
{
    std::string ip;
    uint16_t port{};
    std::vector<char> security_key;
};

enum Screens {
    SCREEN_CONNECTION = 0,
    SCREEN_MAIN = 1,
    SCREEN_FILE_DIALOG = 2,
    SCREEN_SETTINGS = 3
};

// --- Cross-platform clipboard utility ---
#include "clip.h"

void copy_to_clipboard(const std::string& text) {
    clip::set_text(text);
}

int main() {
    auto screen = ScreenInteractive::Fullscreen();
    int active_screen = SCREEN_CONNECTION;
    int settings_previous_screen = SCREEN_CONNECTION;
    int file_dialog_previous_screen = SCREEN_MAIN;

     auto peerInfo = PeerInformation();

    asio::io_context io_ctx;
    std::vector<unsigned char> encryption_key = crypto::generate_key();

    enum FileExplorerMode {
        EXPLORER_SEND_FILE = 0,
        EXPLORER_SELECT_DIR = 1
    };
    FileExplorerMode explorer_mode = EXPLORER_SEND_FILE;

    auto get_home_dir = []() -> std::string {
        if (const char* home = getenv("HOME")) return home;
        if (const char* userprofile = getenv("USERPROFILE")) return userprofile;
        const char* homedrive = getenv("HOMEDRIVE");
        const char* homepath = getenv("HOMEPATH");
        if (homedrive && homepath) return std::string(homedrive) + std::string(homepath);
        return "";
    };

    // --- Settings State ---
    std::string home = get_home_dir();
    std::string settings_download_dir = (fs::path(home) / "Documents" / "FileLighting").string();
    auto load_settings = [&]() {
        if (home.empty()) return;
        fs::path settings_dir = fs::path(home) / "Documents" / "FileLighting";

        if (fs::path settings_file = settings_dir / "settings.json"; fs::exists(settings_file)) {
            std::ifstream in(settings_file);
            std::string line;
            while (std::getline(in, line)) {
                auto pos = line.find("\"download_directory\"");
                if (pos != std::string::npos) {
                    auto colon = line.find(':', pos);
                    if (colon != std::string::npos) {
                        auto quote1 = line.find('\"', colon);
                        if (quote1 != std::string::npos) {
                            auto quote2 = line.find('\"', quote1 + 1);
                            if (quote2 != std::string::npos) {
                                settings_download_dir = line.substr(quote1 + 1, quote2 - quote1 - 1);
                            }
                        }
                    }
                }
            }
        }
    };
    auto save_settings = [&]() {
        std::string home = get_home_dir();
        if (home.empty()) return;
        fs::path settings_dir = fs::path(home) / "Documents" / "FileLighting";
        std::error_code ec;
        fs::create_directories(settings_dir, ec);

        fs::path settings_file = settings_dir / "settings.json";
        std::ofstream out(settings_file);
        out << "{\n";
        out << "  \"download_directory\": \"" << settings_download_dir << "\"\n";
        out << "}\n";
    };
    load_settings();

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
    std::string previous_search_input;

    int selected_file_index = 0;
    int last_confirmed_index = -1;

    std::vector<std::string> chat_history;
    std::map<uint32_t, FileTransfer> file_transfers;
    std::vector<std::string> all_directory_files;
    std::vector<std::string> current_directory_files;

    // --- API Functions for your own system ---
    auto api_update_secure_key = [&](const std::string& new_key) {
        my_code = new_key;
    };

    auto api_add_chat_message = [&](const std::string& who, const std::string& message) {
        chat_history.push_back(who + ": " + message);
    };

    auto api_update_file_transfer = [&](uint32_t file_id, const std::string& file_path, size_t total_size, size_t current_size) {
        file_transfers[file_id] = {file_id, file_path, total_size, current_size};
    };

    auto api_remove_file_transfer = [&](uint32_t file_id) {
        file_transfers.erase(file_id);
    };
    // -----------------------------------------

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
        all_directory_files.emplace_back("..");

        try {
            std::vector<std::string> files;
            std::vector<std::string> directories;
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
            all_directory_files.emplace_back("<Access Denied>");
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
    auto toggle_connection = [&] {
        if (!is_connecting)
        {
            if (peer_code.empty()) return;

            std::vector<unsigned char> decoded_bin(peer_code.size());
            size_t decoded_len = 0;
            if (sodium_base642bin(
                decoded_bin.data(), decoded_bin.size(),
                peer_code.data(), peer_code.size(),
                " \n\r\t", &decoded_len, nullptr,
                sodium_base64_VARIANT_URLSAFE) != 0) 
            {
                // Failed to decode
                return;
            }
            
            std::string peer_decoded(reinterpret_cast<char*>(decoded_bin.data()), decoded_len);

            size_t first_colon = peer_decoded.find(':');
            size_t second_colon = peer_decoded.find(':', first_colon + 1);

            if (first_colon != std::string::npos && second_colon != std::string::npos) {
                peerInfo.ip = peer_decoded.substr(0, first_colon);
                try {
                    peerInfo.port = static_cast<uint16_t>(std::stoul(peer_decoded.substr(first_colon + 1, second_colon - first_colon - 1)));
                } catch (...) {
                    return; // Invalid port
                }
                
                std::string key_str = peer_decoded.substr(second_colon + 1);
                peerInfo.security_key.assign(key_str.begin(), key_str.end());
            } else {
                return; // Invalid format
            }

            is_connecting = true;
            connect_btn_label = "Cancel";
        }
        else if (is_connecting)
        {
            is_connecting = false;
            connect_btn_label = "Connect";
        }
    };

    InputOption peer_input_option;
    peer_input_option.multiline = false;
    peer_input_option.on_enter = toggle_connection;

    std::thread start_ping_thread([&]
    {
        while (true)
        {
            if (!is_connecting)
            {
                if (auto endpoint = stun::fetch_public_endpoint(io_ctx))
                {
                    std::string secure_key = std::format("{}:{}:{}",
                        endpoint->ip,
                        endpoint->port,
                        std::string_view(reinterpret_cast<const char*>(encryption_key.data()), encryption_key.size())
                    );

                    size_t b64_len = sodium_base64_encoded_len(secure_key.size(), sodium_base64_VARIANT_URLSAFE);
                    std::string base64_key(b64_len, '\0');

                    sodium_bin2base64(
                        base64_key.data(), base64_key.size(),
                        reinterpret_cast<const unsigned char*>(secure_key.data()), secure_key.size(),
                        sodium_base64_VARIANT_URLSAFE
                    );

                    if (!base64_key.empty() && base64_key.back() == '\0') {
                        base64_key.pop_back();
                    }

                    api_update_secure_key(base64_key);
                    screen.PostEvent(Event::Custom);
                    std::this_thread::sleep_for(std::chrono::seconds(25));
                }
                else
                {
                    break;
                }
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }
    });

    start_ping_thread.detach();

    Component input_peer_code = Input(&peer_code, "Enter peer code...", peer_input_option);
    Component conditional_input = Maybe(input_peer_code, [&] { return !is_connecting; });

    Component btn_connect_cancel = Button(&connect_btn_label, toggle_connection, safe_button);

    Component btn_settings_conn_raw = Button("Settings", [&] {
        settings_previous_screen = active_screen;
        active_screen = SCREEN_SETTINGS;
    }, safe_button);
    Component btn_settings_conn = Maybe(btn_settings_conn_raw, [&] { return !is_connecting; });

    Component btn_force_start = Button("Force Start (Simulate Success)", [&] {
        active_screen = SCREEN_MAIN;
    }, safe_button);

    Component btn_copy = Button("Copy", [&] {
        copy_to_clipboard(my_code);
    }, safe_button);

    auto connection_layout = Container::Vertical({
        btn_copy,
        conditional_input,
        btn_connect_cancel,
        btn_settings_conn,
        btn_force_start
    });

    auto connection_renderer = Renderer(connection_layout, [&] {
        auto status_text = is_connecting
            ? text("Status: Connecting to " + peerInfo.ip + ":" + std::to_string(peerInfo.port) + " Waiting for hole punch") | color(Color::Yellow)
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
        
        if (!is_connecting) {
            bottom_elements.push_back(btn_settings_conn_raw->Render() | size(WIDTH, EQUAL, 20) | hcenter);
        }

        return window(text(" P2P Setup "),
            vbox({
                filler(),
                vbox({
                    text("Share this secure code with your peer:") | hcenter,
                    hbox({
                        text(my_code) | bold | color(Color::Cyan) | vcenter,
                        text("   ") | vcenter,
                        btn_copy->Render()
                    }) | hcenter,
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
            // TODO: Hook up your network send message here!
            // e.g. send_message_over_network(chat_input);
            
            // You can optionally add it to history here immediately, or wait for network ACK:
            // api_add_chat_message("You", chat_input);
            
            chat_input.clear();
        }
    };
    Component input_chat = Input(&chat_input, "Type message...", chat_input_option);

    Component btn_browse = Button("Browse Files & Folders", [&] {
        explorer_mode = EXPLORER_SEND_FILE;
        file_dialog_previous_screen = active_screen;
        active_screen = SCREEN_FILE_DIALOG;
    }, safe_button);

    Component btn_settings_main = Button("Settings", [&] {
        settings_previous_screen = active_screen;
        active_screen = SCREEN_SETTINGS;
    }, safe_button);

    Component btn_exit = Button("Exit Application", screen.ExitLoopClosure(), safe_button);

    auto chat_container = Container::Vertical({ input_chat });
    auto sidebar_actions = Container::Vertical({ btn_browse, btn_settings_main, btn_exit });

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
        for (const auto& [id, transfer] : file_transfers) {
            float progress = transfer.total_size > 0 ? (float)transfer.current_size / transfer.total_size : 0.0f;
            std::string percent_str = std::to_string(progress * 100);
            percent_str = percent_str.substr(0, percent_str.find('.') + 2); // keep 1 decimal
            std::string label = fs::path(transfer.file_path).filename().string() + " (" + percent_str + "%)";
            
            elements.push_back(hbox({
                text(label) | flex,
                gauge(progress) | size(WIDTH, EQUAL, 20)
            }));
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
                    btn_settings_main->Render(),
                    filler(),
                    btn_exit->Render()
                })
            ) | size(HEIGHT, EQUAL, 12)
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
            std::string home = get_home_dir();
            if (!home.empty()) {
                target.replace(0, 1, home);
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
                if (explorer_mode == EXPLORER_SEND_FILE) {
                    // TODO: Hook up your network file send here!
                    std::string full_path = (fs::path(path_input) / selected_item).string();
                    // e.g. start_sending_file(full_path);
                    
                    last_confirmed_index = -1;
                    active_screen = file_dialog_previous_screen;
                }
            }
        } else {
            last_confirmed_index = selected_file_index;
        }
    };

    Component file_menu = Menu(&current_directory_files, &selected_file_index, file_menu_option);

    Component btn_cancel = Button("Cancel", [&] { active_screen = file_dialog_previous_screen; }, safe_button);
    Component btn_confirm = Button("Confirm", [&] {
        if (explorer_mode == EXPLORER_SEND_FILE) {
            if (last_confirmed_index != -1 && last_confirmed_index < current_directory_files.size()) {
                std::string selected_item = current_directory_files[last_confirmed_index];
                if (selected_item.back() == '/') selected_item.pop_back();

                // TODO: Hook up your network file send here!
                std::string full_path = (fs::path(path_input) / selected_item).string();
                // e.g. start_sending_file(full_path);
            }
        } else {
            std::string chosen_dir = path_input;
            if (last_confirmed_index != -1 && last_confirmed_index < current_directory_files.size()) {
                std::string selected_item = current_directory_files[last_confirmed_index];
                if (selected_item.back() == '/') {
                    selected_item.pop_back();
                    chosen_dir = (fs::path(path_input) / selected_item).string();
                }
            }
            settings_download_dir = chosen_dir;
        }
        active_screen = file_dialog_previous_screen;
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
    // SCREEN 4: SETTINGS
    // ==========================================
    InputOption download_dir_option;
    download_dir_option.multiline = false;
    Component input_download_dir = Input(&settings_download_dir, "Download Directory...", download_dir_option);

    Component btn_browse_download_dir = Button("Browse...", [&] {
        explorer_mode = EXPLORER_SELECT_DIR;
        file_dialog_previous_screen = active_screen;
        active_screen = SCREEN_FILE_DIALOG;
    }, safe_button);

    Component btn_default_download_dir = Button("Default", [&] {
        std::string home = get_home_dir();
        settings_download_dir = (fs::path(home) / "Downloads" / "FileLighting").string();
    }, safe_button);

    Component btn_save_settings = Button("Save Settings", [&] {
        save_settings();
    }, safe_button);

    Component btn_close_settings = Button("Back", [&] {
        active_screen = settings_previous_screen;
    }, safe_button);

    auto dir_layout = Container::Horizontal({
        input_download_dir,
        btn_default_download_dir,
        btn_browse_download_dir
    });

    auto buttons_layout = Container::Horizontal({
        btn_close_settings,
        btn_save_settings
    });

    auto settings_layout = Container::Vertical({
        dir_layout,
        buttons_layout
    });

    auto settings_renderer = Renderer(settings_layout, [&] {
        return window(text(" Settings "),
            vbox({
                text("Settings Configuration") | hcenter | bold,
                separator(),
                window(text(" Download Directory "),
                    hbox({
                        input_download_dir->Render() | vcenter | flex,
                        btn_default_download_dir->Render(),
                        btn_browse_download_dir->Render()
                    })
                ),
                filler(),
                separator(),
                hbox({
                    btn_close_settings->Render(),
                    btn_save_settings->Render()
                }) | hcenter
            })
        ) | flex;
    });

    // ==========================================
    // MASTER CONTROLLER
    // ==========================================
    auto master_tabs = Container::Tab({
        connection_renderer,
        main_screen_renderer,
        file_dialog_renderer,
        settings_renderer
    }, &active_screen);

    auto global_renderer = CatchEvent(master_tabs, [&](Event event) {
        if (event == Event::Escape) {
            screen.ExitLoopClosure()();
            return true;
        }
        return false;
    });

    screen.Loop(global_renderer);
    return 0;
}