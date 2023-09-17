#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wlr-gamma-control-unstable-v1.h>

#include "assert.hpp"
#include "ipc.hpp"

struct Color {
    double red   = 1.0;
    double green = 1.0;
    double blue  = 1.0;
    double gamma = 1.0;

    auto operator==(const Color& o) -> bool {
        return red == o.red &&
               green == o.green &&
               blue == o.blue &&
               gamma == o.gamma;
    }
};

struct Output {
    wl_output*             data          = nullptr;
    zwlr_gamma_control_v1* gamma_control = nullptr;
    uint32_t               registry_name;
    uint32_t               gamma_size;
    int                    ipc_fd = 0;
    std::string            name;
    Color                  color;

    static auto create_gamma_table(const uint32_t table_size) -> std::pair<int, uint16_t*> {
        auto filename = std::array<char, 32>();
        sprintf(filename.data(), "darker-%u", getpid());
        auto fd = memfd_create(filename.data(), 0);
        DYN_ASSERT(fd >= 0);
        DYN_ASSERT(ftruncate(fd, table_size) == 0);

        auto data = (uint16_t*)mmap(NULL, table_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        DYN_ASSERT(data != MAP_FAILED);
        return {fd, data};
    }

    static auto fill_gamma_table(uint16_t* const table, const uint32_t ramp_size, const Color& color) -> void {
        const auto r = table;
        const auto g = r + ramp_size;
        const auto b = g + ramp_size;
        for(auto i = 0u; i < ramp_size; i += 1) {
            const auto e = 1. * i / (ramp_size - 1);

            r[i] = UINT16_MAX * pow(e * color.red, 1.0 / color.gamma);
            g[i] = UINT16_MAX * pow(e * color.green, 1.0 / color.gamma);
            b[i] = UINT16_MAX * pow(e * color.blue, 1.0 / color.gamma);
        }
    }

    auto set_gamma_table(const Color new_color) -> void {
        if(color == new_color) {
            return;
        }
        const auto [fd, mem] = create_gamma_table(gamma_size * 3 * 2);
        fill_gamma_table(mem, gamma_size, new_color);
        lseek(fd, 0, SEEK_SET);
        zwlr_gamma_control_v1_set_gamma(gamma_control, fd);
        munmap(mem, gamma_size * 3 * 2);
        close(fd);
        color = new_color;
    }

    auto finish() -> void {
        if(data != nullptr) {
            wl_output_release(data);
            data = nullptr;
        }
        if(gamma_control != nullptr) {
            zwlr_gamma_control_v1_destroy(gamma_control);
            gamma_control = nullptr;
        }
        if(ipc_fd != 0) {
            close(ipc_fd);
            ipc_fd = 0;
        }
        remove(name.data());
    }
};

struct Context {
    std::vector<Output>            outputs;
    zwlr_gamma_control_manager_v1* gamma_control_manager;

    auto find_output(wl_output* output) -> Output& {
        for(auto& o : outputs) {
            if(o.data == output) {
                return o;
            }
        }
        PANIC();
    }

    auto find_output(zwlr_gamma_control_v1* gamma_control) -> Output& {
        for(auto& o : outputs) {
            if(o.gamma_control == gamma_control) {
                return o;
            }
        }
        PANIC();
    }
};

namespace {
auto parse_double(const char* const str) -> double {
    const auto r = strtod(str, NULL);
    DYN_ASSERT(errno == 0);
    return r;
}

auto write_int_to_file(const char* const path, const int value) -> void {
    auto fd = open(path, O_WRONLY | O_CREAT, 0644);
    DYN_ASSERT(fd >= 0, path, fd, errno);
    const auto str = std::to_string(value);
    DYN_ASSERT(write(fd, str.data(), str.size()) == str.size());
    close(fd);
}

auto read_int_from_file(const char* const path) -> int {
    auto r  = -1;
    auto fd = -1;
    do {
        fd = open(path, O_RDONLY);
        if(fd < 0) {
            break;
        }

        auto buf = std::array<char, 64>();
        if(read(fd, buf.data(), 64) < 0) {
            break;
        }

        r = strtol(buf.data(), NULL, 0);
        if(errno != 0) {
            break;
        }
    } while(0);
    if(fd != -1) {
        close(fd);
    }
    return r;
}

// wayland stuff
auto empty_callback() -> void {
    return;
}

// zwlr_gamma_control_manager_v1
auto zwlr_gamma_control_v1_gamma_size(void* const data, zwlr_gamma_control_v1* const gamma_control, const uint32_t size) -> void {
    auto& ctx    = *std::bit_cast<Context*>(data);
    auto& o      = ctx.find_output(gamma_control);
    o.gamma_size = size;

    // if(o.name == "eDP-1") {
    //     o.set_gamma_table({0.5, 0.5, 0.5, 1});
    // }
    write_int_to_file(o.name.data(), o.color.red * 100);
    o.ipc_fd = ipc::create(o.name.data());
}

auto zwlr_gamma_control_v1_failed(void* const /*data*/, zwlr_gamma_control_v1* const /*gamma_control_v1*/) -> void {
    warn("control failed\n");
}

auto gamma_control_v1_listener = zwlr_gamma_control_v1_listener{
    .gamma_size = zwlr_gamma_control_v1_gamma_size,
    .failed     = zwlr_gamma_control_v1_failed,
};

// output
auto output_name(void* const data, wl_output* const output, const char* const name) -> void {
    printf("output: %s\n", name);
    auto& ctx = *std::bit_cast<Context*>(data);
    auto& o   = ctx.find_output(output);
    o.name    = name;

    const auto gamma_control = zwlr_gamma_control_manager_v1_get_gamma_control(ctx.gamma_control_manager, output);
    zwlr_gamma_control_v1_add_listener(gamma_control, &gamma_control_v1_listener, data);
    o.gamma_control = gamma_control;
}

auto output_listener = wl_output_listener{
    .geometry    = (decltype(wl_output_listener::geometry))empty_callback,
    .mode        = (decltype(wl_output_listener::mode))empty_callback,
    .done        = (decltype(wl_output_listener::done))empty_callback,
    .scale       = (decltype(wl_output_listener::scale))empty_callback,
    .name        = output_name,
    .description = (decltype(wl_output_listener::description))empty_callback,
};

// registory
auto registry_global(void* const data, wl_registry* const registry, const uint32_t name, const char* const interface, const uint32_t version) -> void {
    auto& ctx = *std::bit_cast<Context*>(data);
    if(strcmp(interface, wl_output_interface.name) == 0) {
        const auto output = (wl_output*)wl_registry_bind(registry, name, &wl_output_interface, WL_OUTPUT_NAME_SINCE_VERSION);
        ctx.outputs.push_back({.data = output, .registry_name = name});
        wl_output_add_listener(output, &output_listener, data);
    } else if(strcmp(interface, zwlr_gamma_control_manager_v1_interface.name) == 0) {
        const auto gamma_control_manager = (zwlr_gamma_control_manager_v1*)(wl_registry_bind(registry, name, &zwlr_gamma_control_manager_v1_interface, 1));
        ctx.gamma_control_manager        = gamma_control_manager;
    }
}

auto registry_global_remove(void* const data, wl_registry* const registry, const uint32_t name) -> void {
    auto& ctx = *std::bit_cast<Context*>(data);
    for(auto i = ctx.outputs.begin(); i < ctx.outputs.end(); i += 1) {
        if(i->registry_name == name) {
            i->finish();
            ctx.outputs.erase(i);
            return;
        }
    }
}

auto registry_listener = wl_registry_listener{
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

} // namespace

auto main(int argc, char* argv[]) -> int {
    auto display = wl_display_connect(nullptr);
    DYN_ASSERT(display != nullptr);
    auto registry = wl_display_get_registry(display);
    DYN_ASSERT(registry != nullptr);

    auto context = Context();

    wl_registry_add_listener(registry, &registry_listener, &context);

    auto display_fd = wl_display_get_fd(display);
loop:
    while(wl_display_prepare_read(display) != 0) {
        wl_display_dispatch_pending(display);
    }
    wl_display_flush(display);

    auto pollfds = std::vector<pollfd>();
    pollfds.push_back({display_fd, POLLIN, 0});
    for(const auto& o : context.outputs) {
        if(o.ipc_fd == 0) {
            continue;
        }
        pollfds.push_back({o.ipc_fd, POLLIN, 0});
    }

    DYN_ASSERT(poll(pollfds.data(), pollfds.size(), -1) != -1);
    if(pollfds[0].revents & POLLIN) {
        wl_display_read_events(display);
        wl_display_dispatch_pending(display);
    } else {
        wl_display_cancel_read(display);
    }
    for(auto i = 1; i < pollfds.size(); i += 1) {
        if(pollfds[i].revents & POLLIN) {
            ipc::read(pollfds[i].fd);

            auto&      output     = context.outputs[i - 1];
            const auto brightness = read_int_from_file(output.name.data());
            if(brightness == -1) {
                warn("failed to read value from ", output.name);
            } else {
                const auto c = brightness / 100.0;
                output.set_gamma_table({c, c, c, 1});
            }
        }
    }
    goto loop;

    wl_display_disconnect(display);

    return 0;
}
