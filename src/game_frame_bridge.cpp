#include "game_frame_bridge.hpp"

#include "util.hpp"

#include <Windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace modelcustom::game_frame
{
namespace
{


template <typename... Args>
class FwEventView
{
public:
    using Function = std::function<bool(Args...)>;

private:
    struct EventCallback
    {
        Function function;
        std::unique_ptr<EventCallback> next = nullptr;
        int order = 0;
        std::size_t cookie = static_cast<std::size_t>(-1);

        explicit EventCallback(Function value)
            : function(std::move(value))
        {
        }

        ~EventCallback()
        {
            while (next)
            {
                next = std::move(next->next);
            }
        }
    };

    std::unique_ptr<EventCallback> callbacks_;
    std::atomic<std::size_t> connectCookie_ = 0;

public:
    std::size_t connect(Function function, int order = 0)
    {
        if (!function)
        {
            return static_cast<std::size_t>(-1);
        }

        const auto cookie = connectCookie_++;
        auto callback = std::make_unique<EventCallback>(std::move(function));
        callback->order = order;
        callback->cookie = cookie;

        if (!callbacks_)
        {
            callbacks_ = std::move(callback);
            return cookie;
        }

        auto current = &callbacks_;
        EventCallback* last = nullptr;
        while (*current && order >= (*current)->order)
        {
            last = current->get();
            current = &(*current)->next;
        }

        callback->next = std::move(*current);
        (!last ? callbacks_ : last->next) = std::move(callback);
        return cookie;
    }

    void disconnect(std::size_t cookie)
    {
        if (cookie == static_cast<std::size_t>(-1))
        {
            return;
        }

        EventCallback* previous = nullptr;
        for (auto current = callbacks_.get(); current; current = current->next.get())
        {
            if (current->cookie == cookie)
            {
                if (previous)
                {
                    previous->next = std::move(current->next);
                }
                else
                {
                    callbacks_ = std::move(current->next);
                }
                return;
            }
            previous = current;
        }
    }
};

static_assert(sizeof(FwEventView<>) == 16);

constexpr char kMainGameFrameExport[] = "?OnMainGameFrame@@3V?$fwEvent@$$V@@A";
FwEventView<>* g_event = nullptr;
std::size_t g_cookie = static_cast<std::size_t>(-1);
}

bool connect(Callback callback)
{
    if (connected())
    {
        return true;
    }
    if (!callback)
    {
        return false;
    }

    const HMODULE module = GetModuleHandleW(L"rage-nutsnbolts-five.dll");
    if (!module)
    {
        return false;
    }

    auto* event = reinterpret_cast<FwEventView<>*>(
        GetProcAddress(module, kMainGameFrameExport));
    if (!event)
    {
        log_error("OnMainGameFrame export was not found");
        return false;
    }

    const auto cookie = event->connect([callback]()
    {
        return callback();
    });
    if (cookie == static_cast<std::size_t>(-1))
    {
        return false;
    }

    g_event = event;
    g_cookie = cookie;
    log("Connected directly to FiveM OnMainGameFrame; ScriptHookV scheduler is unused");
    adhesive_diag(
        "frame_bridge.connect",
        "event=" + adhesive_diag_address(event) +
        " cookie=" + std::to_string(cookie) +
        " sh_allowed_write=0 scripthook_register=0");
    return true;
}

void disconnect()
{
    if (!connected())
    {
        return;
    }

    g_event->disconnect(g_cookie);
    adhesive_diag(
        "frame_bridge.disconnect",
        "event=" + adhesive_diag_address(g_event) +
        " cookie=" + std::to_string(g_cookie));
    g_event = nullptr;
    g_cookie = static_cast<std::size_t>(-1);
}

bool connected()
{
    return g_event != nullptr && g_cookie != static_cast<std::size_t>(-1);
}
}
