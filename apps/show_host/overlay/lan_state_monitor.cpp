#include "lan_state_monitor.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <atomic>
#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

struct LanStateMonitor::Impl {
    std::atomic<HWND> window{};
    std::atomic<bool> pending{};
    UINT message{};
    HANDLE interfaces{}, addresses{};
    void notify() noexcept {
        const auto hwnd = window.load();
        if (hwnd && !pending.exchange(true) && !PostMessageW(hwnd, message, 0, 0)) pending.store(false);
    }
    static void CALLBACK interface_changed(void* p, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) {
        static_cast<Impl*>(p)->notify();
    }
    static void CALLBACK address_changed(void* p, PMIB_UNICASTIPADDRESS_ROW, MIB_NOTIFICATION_TYPE) {
        static_cast<Impl*>(p)->notify();
    }
};
LanStateMonitor::LanStateMonitor() : impl_(std::make_unique<Impl>()) {}
LanStateMonitor::~LanStateMonitor() { stop(); }
void LanStateMonitor::start(void* window, std::uint32_t message) {
    stop();
    impl_->message = message;
    impl_->window.store(static_cast<HWND>(window));
    NotifyIpInterfaceChange(AF_INET, Impl::interface_changed, impl_.get(), FALSE, &impl_->interfaces);
    NotifyUnicastIpAddressChange(AF_INET, Impl::address_changed, impl_.get(), FALSE, &impl_->addresses);
}
void LanStateMonitor::stop() noexcept {
    impl_->window.store(nullptr);
    if (impl_->interfaces) CancelMibChangeNotify2(impl_->interfaces);
    if (impl_->addresses) CancelMibChangeNotify2(impl_->addresses);
    impl_->interfaces = impl_->addresses = nullptr;
    impl_->pending.store(false);
}
bool LanStateMonitor::usable_ipv4(std::uint32_t ip) noexcept {
    const auto a = ip >> 24, b = (ip >> 16) & 255;
    return a != 0 && a != 127 && a < 224 && !(a == 169 && b == 254)
        && !(a == 198 && (b == 18 || b == 19))
        && !(a == 100 && b >= 64 && b <= 127);
}
bool LanStateMonitor::usable_adapter(std::wstring_view name, std::uint32_t type, bool up) {
    if (!up || type == IF_TYPE_SOFTWARE_LOOPBACK || type == IF_TYPE_TUNNEL) return false;
    std::wstring lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return std::towlower(c); });
    static constexpr std::wstring_view excluded[]{L"virtual", L"vmware", L"vmnet",
        L"hyper-v", L"vethernet", L"veth", L"wsl", L"docker", L"vpn", L"radmin",
        L"mihomo", L"meta tunnel", L"tun", L"tap", L"loopback", L"bluetooth",
        L"tailscale", L"zerotier", L"wireguard", L"hamachi", L"teredo", L"6to4",
        L"ip-https", L"miniport", L"wi-fi direct", L"wifi direct", L"本地连接*", L"内核调试器"};
    for (const auto token : excluded) if (lower.find(token) != std::wstring::npos) return false;
    return true;
}
bool LanStateMonitor::refresh() {
    impl_->pending.store(false);
    ULONG size = 16 * 1024;
    std::vector<unsigned char> bytes(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(bytes.data());
    auto result = GetAdaptersAddresses(AF_INET,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, adapters, &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        bytes.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(bytes.data());
        result = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, adapters, &size);
    }
    if (result != NO_ERROR) return false;
    for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
        if (!usable_adapter(adapter->FriendlyName ? adapter->FriendlyName : L"",
                adapter->IfType, adapter->OperStatus == IfOperStatusUp)) continue;
        for (auto* address = adapter->FirstUnicastAddress; address; address = address->Next) {
            if (!address->Address.lpSockaddr || address->Address.lpSockaddr->sa_family != AF_INET
                    || address->DadState == IpDadStateDuplicate || address->DadState == IpDadStateTentative) continue;
            const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address->Address.lpSockaddr);
            if (usable_ipv4(ntohl(ipv4->sin_addr.s_addr))) return true;
        }
    }
    return false;
}
