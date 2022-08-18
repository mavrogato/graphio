
#include <iostream>
#include <algorithm>
#include <coroutine>
#include <concepts>
#include <memory>
#include <list>
#include <vector>
#include <tuple>
#include <string>
#include <filesystem>
#include <complex>
#include <numbers>
#include <ranges>

#include <CL/sycl.hpp>

#include <wayland-client.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <linux/input-event-codes.h>

inline namespace coroutines
{
    template <class T>
    struct delay {
        struct promise_type {
            std::coroutine_handle<> continuation;
            T value;
            void unhandled_exception() { throw; }
            auto get_return_object() noexcept { return delay{*this}; }
            auto initial_suspend() noexcept { return std::suspend_always{}; }
            auto final_suspend() noexcept {
                struct awaiter : std::suspend_always {
                    auto await_suspend(std::coroutine_handle<promise_type> handle) const noexcept
                        -> std::coroutine_handle<>
                    {
                        /*
                          awaiter::await_suspend is called when the execution of the
                          current coroutine (referred to by 'handle') is about to finish.
                          If the current coroutine was resumed by another coroutine via
                          co_await get_delay(), a handle to that coruitne has been stored
                          as handle.promise().continuation. In that case, return the handle
                          to resume the previous coroutine.
                          Otherwise, return noop_coroutine(), whose resumption does nothing.
                        */
                        if (auto continuation = handle.promise().continuation) {
                            return continuation;
                        }
                        return std::noop_coroutine();
                    }
                };
                return awaiter{};
            }
            void return_value(T value) noexcept { this->value = std::move(value); }
        };
        std::coroutine_handle<promise_type> handle;
        ~delay() noexcept {
            if (this->handle) this->handle.destroy();
        }
        delay(delay const&) = delete;
        delay(delay&& rhs) noexcept
            : handle{std::exchange(rhs.handle, nullptr)}
        {
        }
        auto operator co_await() noexcept {
            struct awaiter : std::suspend_always {
                std::coroutine_handle<promise_type> handle;
                awaiter(std::coroutine_handle<promise_type> handle) : handle{handle}
                {
                }
                T await_resume() noexcept { return std::move(this->handle.promise().value); }
                auto await_suspend(std::coroutine_handle<> handle) noexcept {
                    this->handle.promise().continuation = handle;
                    return this->handle;
                }
            };
            return awaiter{this->handle};
        }
        T operator()() noexcept {
            this->handle.resume();
            return std::move(this->handle.promise().value);
        }
    private:
        explicit delay(promise_type& p) noexcept
            : handle{std::coroutine_handle<promise_type>::from_promise(p)}
        {
        }
    };
} // ::coroutines

inline namespace wayland_client_helper
{
    template <class> constexpr std::nullptr_t wl_interface_ptr = nullptr;
#define INTERN_WL_INTERFACE(wlx)                                        \
    template <> constexpr wl_interface const *const wl_interface_ptr<wlx> = &wlx##_interface;
    INTERN_WL_INTERFACE(wl_display);
    INTERN_WL_INTERFACE(wl_registry);
    INTERN_WL_INTERFACE(wl_compositor);
    INTERN_WL_INTERFACE(wl_shell);
    INTERN_WL_INTERFACE(wl_seat);
    INTERN_WL_INTERFACE(wl_keyboard);
    INTERN_WL_INTERFACE(wl_pointer);
    INTERN_WL_INTERFACE(wl_touch);
    INTERN_WL_INTERFACE(wl_shm);
    INTERN_WL_INTERFACE(wl_surface);
    INTERN_WL_INTERFACE(wl_shell_surface);
    INTERN_WL_INTERFACE(wl_buffer);
    INTERN_WL_INTERFACE(wl_shm_pool);
    INTERN_WL_INTERFACE(wl_callback);
    INTERN_WL_INTERFACE(wl_output);
#undef INTERN_WL_INTERFACE
    template <class T>
    concept wl_client_t = std::same_as<decltype (wl_interface_ptr<T>), wl_interface const *const>;
    template <wl_client_t T, class Ch>
    auto& operator << (std::basic_ostream<Ch>& output, T const* ptr) noexcept {
        return output << static_cast<void const*>(ptr)
                      << '['
                      << wl_interface_ptr<T>->name
                      << ']';
    }
    template <wl_client_t T>
    [[nodiscard]] auto attach_unique(T* ptr) noexcept {
        static constexpr auto deleter = [](T* ptr) noexcept -> void {
            std::cout << ptr << " deleting." << std::endl;
            static constexpr auto interface_addr = wl_interface_ptr<T>;
            if      constexpr (interface_addr == std::addressof(wl_display_interface)) {
                wl_display_disconnect(ptr);
            }
            else if constexpr (interface_addr == std::addressof(wl_keyboard_interface)) {
                wl_keyboard_release(ptr);
            }
            else if constexpr (interface_addr == std::addressof(wl_pointer_interface)) {
                wl_pointer_release(ptr);
            }
            else if constexpr (interface_addr == std::addressof(wl_touch_interface)) {
                wl_touch_release(ptr);
            }
            else {
                wl_proxy_destroy(reinterpret_cast<wl_proxy*>(ptr));
            }
        };
        return std::unique_ptr<T, decltype (deleter)>(ptr, deleter);
    }
    template <wl_client_t T>
    using unique_ptr_t = decltype (attach_unique(std::declval<T*>()));
} // ::wayland_client_helper

[[nodiscard]]
inline auto create_shm_buffer(wl_shm* shm, size_t cx, size_t cy, uint32_t** pixels) noexcept
-> wl_buffer*
{
    // Check the environment
    std::string_view xdg_runtime_dir = std::getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime_dir.empty() || !std::filesystem::exists(xdg_runtime_dir)) {
        std::cerr << "This program requires XDG_RUNTIME_DIR setting..." << std::endl;
        return nullptr;
    }
    std::string_view tmp_file_title = "/weston-shared-XXXXXX";
    if (1024 <= xdg_runtime_dir.size() + tmp_file_title.size()) {
        std::cerr << "The path of XDG_RUNTIME_DIR is too long..." << std::endl;
        return nullptr;
    }
    char tmp_path[1024] = { };
    auto p = std::strcat(tmp_path, xdg_runtime_dir.data());
    std::strcat(p, tmp_file_title.data());
    int fd = mkostemp(tmp_path, O_CLOEXEC);
    if (fd >= 0) {
        unlink(tmp_path);
    }
    else {
        std::cerr << "mkostemp failed..." << std::endl;
        return nullptr;
    }
    if (ftruncate(fd, 4*cx*cy) < 0) {
        std::cerr << "ftruncate failed..." << std::endl;
        close(fd);
        return nullptr;
    }
    auto data = mmap(nullptr, 4*cx*cy, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        std::cerr << "mmap failed..." << std::endl;
        close(fd);
        return nullptr;
    }
    *pixels = reinterpret_cast<uint32_t*>(data);
    return wl_shm_pool_create_buffer(attach_unique(wl_shm_create_pool(shm, fd, 4*cx*cy)).get(),
                                     0,
                                     cx, cy,
                                     cx * 4,
                                     WL_SHM_FORMAT_ARGB8888);
}

void rendering(sycl::queue& que,
               uint32_t* pixels,
               sycl::range<2> dim,
               std::vector<std::complex<double>> const& vertices) noexcept;

void windowing(wl_display* display, wl_registry* registry, auto const& globals) noexcept {
    constexpr int32_t cx = 1024;
    constexpr int32_t cy =  768;
    wl_compositor* compositor;
    wl_shell* shell;
    wl_seat* seat;
    wl_shm* shm;
    for (auto const& item : globals) {
        auto const& [name, interface, version] = item;
        if (interface == wl_compositor_interface.name) {
            compositor = reinterpret_cast<wl_compositor*>(wl_registry_bind(registry,
                                                                           name,
                                                                           &wl_compositor_interface,
                                                                           version));
        }
        else if (interface == wl_shell_interface.name) {
            shell = reinterpret_cast<wl_shell*>(wl_registry_bind(registry,
                                                                 name,
                                                                 &wl_shell_interface,
                                                                 version));
        }
        else if (interface == wl_seat_interface.name) {
            seat = reinterpret_cast<wl_seat*>(wl_registry_bind(registry,
                                                               name,
                                                               &wl_seat_interface,
                                                               version));
        }
        else if (interface == wl_shm_interface.name) {
            shm = reinterpret_cast<wl_shm*>(wl_registry_bind(registry,
                                                             name,
                                                             &wl_shm_interface,
                                                             version));
        }
    }
    if (!compositor || !shell || !seat || !shm) {
        std::cerr << "Some required globals are missing..." << std::endl;
        return ;
    }
    auto compositor_ptr = attach_unique(compositor);
    auto shell_ptr = attach_unique(shell);
    auto seat_ptr = attach_unique(seat);
    auto shm_ptr = attach_unique(shm);
    std::vector<uint32_t> formats;
    wl_shm_listener shm_listener {
        .format = [](void* data, auto, uint32_t format) noexcept {
            reinterpret_cast<decltype (formats)*>(data)->push_back(format);
        },
    };
    if (wl_shm_add_listener(shm, &shm_listener, &formats)) {
        std::cerr << "wl_shm_add_listener failed..." << std::endl;
        return ;
    }
    uint32_t seat_capability = 0;
    wl_seat_listener seat_listener {
        .capabilities = [](void* data, auto, uint32_t caps) noexcept {
            *reinterpret_cast<uint32_t*>(data) = caps;
        },
        .name = [](auto, auto, char const* name) noexcept {
        }
    };
    if (wl_seat_add_listener(seat, &seat_listener, &seat_capability)) {
        std::cerr << "wl_seat_add_listener failed..." << std::endl;
        return ;
    }
    wl_display_roundtrip(display);
    if (std::none_of(formats.begin(), formats.end(), [](auto item) noexcept {
        return item == WL_SHM_FORMAT_ARGB8888;
    }))
    {
        std::cerr << "WL_SHM_FORMAT_ARGB8888 required..." << std::endl;
        return ;
    }
    if (!(seat_capability & WL_SEAT_CAPABILITY_POINTER) ||
        !(seat_capability & WL_SEAT_CAPABILITY_KEYBOARD))
    {
        std::cerr << "Keyboad and pointers required..." << std::endl;
        return ;
    }
    auto keyboard = wl_seat_get_keyboard(seat);
    if (!keyboard) {
        std::cerr << "wl_seat_get_keyboard failed..." << std::endl;
        return ;
    }
    auto keyboard_ptr = attach_unique(keyboard);
    bool quit = false;
    wl_keyboard_listener keyboard_listener {
        .keymap = [](auto...) noexcept { },
        .enter = [](auto...) noexcept { },
        .leave = [](auto...) noexcept { },
        .key = [](auto data, auto, auto serial, auto time, auto key, auto state) noexcept {
            if (state == 0 && (key == 1 || key == 16)) {
                *reinterpret_cast<bool*>(data) = true;
            }
        },
        .modifiers = [](auto...) noexcept { },
        .repeat_info = [](auto...) noexcept { },
    };
    if (wl_keyboard_add_listener(keyboard, &keyboard_listener, &quit)) {
        std::cerr << "wl_keyboard_add_listener failed..." << std::endl;
        return ;
    }
    auto pointer = wl_seat_get_pointer(seat);
    if (!pointer) {
        std::cerr << "wl_seat_get_pointer failed..." << std::endl;
        return ;
    }
    auto pointer_ptr = attach_unique(pointer);
    std::vector<std::complex<double>> vertices{{}};
    assert(vertices.empty() == false);
    wl_pointer_listener pointer_listener {
        .enter = [](auto...) noexcept { },
        .leave = [](auto...) noexcept { },
        .motion = [](auto data, auto, auto time, auto x, auto y) noexcept {
            auto vertices = reinterpret_cast<std::vector<std::complex<double>>*>(data);
            auto& cursor = vertices->front();
            cursor = { wl_fixed_to_double(x), wl_fixed_to_double(y) };
        },
        .button = [](auto data, auto, auto, auto, auto button, auto state) /*noexcept*/ {
            if (button == BTN_RIGHT && state) {
                constexpr float tau = 2 * std::numbers::pi_v<double>;
                constexpr float phi = std::numbers::phi_v<double>;
                auto vertices = reinterpret_cast<std::vector<std::complex<double>>*>(data);
                auto pt = vertices->front();
                vertices->push_back(pt);
                for (int i = 1; i < 64; ++i) {
                    auto d = std::polar<double>(std::sqrt(i), i*tau*phi);
                    vertices->push_back(pt + d);
                }
                for (int i = 1; i < std::abs(pt); ++i) {
                    vertices->push_back(std::fmod(phi*i, 1.0) * pt);
                }
                for (int i = 1; i < std::abs(std::complex<double>(cx, 0) - pt); ++i) {
                    vertices->push_back(pt +
                                        std::fmod(phi*i, 1.0) * (std::complex<double>(cx, 0) - pt));
                }
            }
        },
        .axis = [](auto...) noexcept { },
        .frame = [](auto...) noexcept { },
        .axis_source = [](auto...) noexcept { },
        .axis_stop = [](auto...) noexcept { },
        .axis_discrete = [](auto...) noexcept { },
    };
    if (wl_pointer_add_listener(pointer, &pointer_listener, &vertices)) {
        std::cerr << "wl_pointer_add_listener failed..." << std::endl;
        return ;
    }
    auto surface = wl_compositor_create_surface(compositor);
    if (!surface) {
        std::cerr << "wl_compositor_create_surface failed..." << std::endl;
        return ;
    }
    auto surface_ptr = attach_unique(surface);
    auto shell_surface = wl_shell_get_shell_surface(shell, surface);
    if (!shell_surface) {
        std::cerr << wl_shell_get_shell_surface(shell, surface) << std::endl;
        return ;
    }
    auto shell_surface_ptr = attach_unique(shell_surface);
    wl_shell_surface_listener shellsurf_listener {
        .ping = [](auto, auto shellsurf, auto serial) noexcept {
            wl_shell_surface_pong(shellsurf, serial);
            std::cout << "Pinged and ponged." << std::endl;
        },
        .configure = [](auto...) noexcept {
            std::cout << "Configuring... (not support yet)" << std::endl;
        },
        .popup_done = [](auto...) noexcept {
            std::cerr << "Popup done." << std::endl;
        },
    };
    if (wl_shell_surface_add_listener(shell_surface, &shellsurf_listener, nullptr)) {
        std::cerr << "wl_shell_surface_add_listener failed..." << std::endl;
        return ;
    }
    uint32_t* pixels = nullptr;
    auto buffer = create_shm_buffer(shm, cx, cy, &pixels);
    if (!buffer) {
        std::cerr << "create_shm_buffer failed..." << std::endl;
        return ;
    }
    auto buffer_ptr = attach_unique(buffer);
    wl_shell_surface_set_toplevel(shell_surface);
    sycl::queue que;
    do {
        if (quit) break;
        rendering(que, pixels, {cy, cx}, vertices);
        wl_surface_damage(surface, 0, 0, cx, cy);
        wl_surface_attach(surface, buffer, 0, 0);
        wl_surface_commit(surface);
        wl_display_flush(display);
    } while (wl_display_dispatch(display) != -1);
}

int main() {
    if (auto display = attach_unique(wl_display_connect(nullptr))) {
        if (auto registry = attach_unique(wl_display_get_registry(display.get()))) {
            try {
                std::list<std::tuple<uint32_t, std::string, uint32_t>> globals;
                wl_registry_listener listener {
                    .global = [](void* data, auto, auto... rest) {
                        auto globals_ptr = reinterpret_cast<decltype (globals)*>(data);
                        globals_ptr->push_back(std::tuple(rest...));
                    },
                    .global_remove = [](void* data, auto, uint32_t name) {
                        auto globals_ptr = reinterpret_cast<decltype (globals)*>(data);
                        std::remove_if(globals_ptr->begin(),
                                       globals_ptr->end(),
                                       [&](auto const& item) noexcept -> bool {
                                           return std::get<0>(item) == name;
                                       });
                    },
                };
                wl_registry_add_listener(registry.get(), &listener, &globals);
                wl_display_roundtrip(display.get());
                windowing(display.get(), registry.get(), globals);
            }
            catch (std::exception& ex) {
                std::cerr << "Exception: " << ex.what() << std::endl;
            }
        }
    }
    return 0;
}

inline uint32_t& assign(uint32_t& lhs, auto a, auto r, auto g, auto b) noexcept {
    uint32_t ret = 0;
    int32_t rhs[] = {
        static_cast<int32_t>(b),
        static_cast<int32_t>(g),
        static_cast<int32_t>(r),
        static_cast<int32_t>(a),
    };
    for (int i = 0; i < 4; ++i) {
        int32_t x = (lhs & (0xff << (i*8))) >> (i*8);
        x = std::min<int>(0xff, x + rhs[i]);
        ret |= (x << (i*8));
    }
    return lhs = ret;
}

void rendering(sycl::queue& que,
               uint32_t* pixels,
               sycl::range<2> dim,
               std::vector<std::complex<double>> const& vertices) noexcept
{
    auto pix = sycl::buffer{pixels, dim};
    auto cur = vertices.front();
    auto pts = sycl::buffer<std::complex<double>>{&vertices.front(), vertices.size()};
    que.submit([&](sycl::handler& h) noexcept {
        auto a = pix.get_access<sycl::access::mode::write>(h);
        h.parallel_for(dim, [=](auto idx) noexcept {
            if (cur.real() == idx[1] || cur.imag() == idx[0]) {
                a[idx] = 0xccffffff;
            }
            else {
                a[idx] = 0xcc000000;
            }
        });
    });
    que.submit([&](sycl::handler& h) noexcept {
        auto apx = pix.get_access<sycl::access::mode::read_write>(h);
        auto apt = pts.get_access<sycl::access::mode::read>(h);
        h.parallel_for(vertices.size(), [=](auto idx) noexcept {
            auto pt = apt[idx];
            auto pq = std::complex<double>{
                std::floor(pt.real()),
                std::floor(pt.imag()),
            };
            auto pr = std::complex<double>{
                std::fmod(pt.real(), 1.0),
                std::fmod(pt.imag(), 1.0),
            };
            auto xr = pr.real();
            auto yr = pr.imag();
            unsigned a = 255 * (1 - xr) * (1 - yr);
            unsigned b = 255 * (1 - xr) * yr;
            unsigned c = 255 * xr * (1 - yr);
            unsigned d = 255 * xr * yr;
            assign(apx[{(unsigned) pq.imag()+0, (unsigned) pq.real()+0}], 0, a, a, a);
            assign(apx[{(unsigned) pq.imag()+1, (unsigned) pq.real()+0}], 0, b, b, b);
            assign(apx[{(unsigned) pq.imag()+0, (unsigned) pq.real()+1}], 0, c, c, c);
            assign(apx[{(unsigned) pq.imag()+1, (unsigned) pq.real()+1}], 0, d, d, d);
            // apx[{(unsigned) pq.imag()+0, (unsigned) pq.real()+0}] |= 0xff000000 | a << 16 | a << 8;
            // apx[{(unsigned) pq.imag()+1, (unsigned) pq.real()+0}] |= 0xff000000 | b << 16 | b << 8;
            // apx[{(unsigned) pq.imag()+0, (unsigned) pq.real()+1}] |= 0xff000000 | c << 16 | c << 8;
            // apx[{(unsigned) pq.imag()+1, (unsigned) pq.real()+1}] |= 0xff000000 | d << 16 | d << 8;
        });
    });
}
