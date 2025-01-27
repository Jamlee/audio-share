/*
   Copyright 2022-2024 mkckr0 <https://github.com/mkckr0>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include "network_manager.hpp"
#include "formatter.hpp"
#include "audio_manager.hpp"

#include <list>
#include <ranges>
#include <coroutine>

#ifdef _WINDOWS
#define NOMINMAX
#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Iphlpapi.lib")
#endif // _WINDOWS

#ifdef linux
#include <sys/types.h>
#include <ifaddrs.h>
#endif

#include <spdlog/spdlog.h>
#include <spdlog/fmt/bin_to_hex.h>
#include <fmt/ranges.h>

namespace ip = asio::ip;
using namespace std::chrono_literals;

network_manager::network_manager(std::shared_ptr<audio_manager>& audio_manager)
    : _audio_manager(audio_manager)
{
}

std::vector<std::string> network_manager::get_address_list()
{
    std::vector<std::string> address_list;

#ifdef _WINDOWS
    ULONG family = AF_INET;
    ULONG flags = GAA_FLAG_INCLUDE_ALL_INTERFACES;

    ULONG size = 0;
    GetAdaptersAddresses(family, flags, nullptr, nullptr, &size);
    auto pAddresses = (PIP_ADAPTER_ADDRESSES)malloc(size);

    auto ret = GetAdaptersAddresses(family, flags, nullptr, pAddresses, &size);
    if (ret == ERROR_SUCCESS) {
        for (auto pCurrentAddress = pAddresses; pCurrentAddress; pCurrentAddress = pCurrentAddress->Next) {
            if (pCurrentAddress->OperStatus != IfOperStatusUp || pCurrentAddress->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
                continue;
            }

            for (auto pUnicast = pCurrentAddress->FirstUnicastAddress; pUnicast; pUnicast = pUnicast->Next) {
                auto sockaddr = (sockaddr_in*)pUnicast->Address.lpSockaddr;
                char buf[50];
                if (inet_ntop(AF_INET, &sockaddr->sin_addr, buf, sizeof(buf))) {
                    address_list.emplace_back(buf);
                }
            }
        }
    }

    free(pAddresses);
#endif

#ifdef linux
    struct ifaddrs* ifaddrs;
    if (getifaddrs(&ifaddrs) == -1) {
        return address_list;
    }

    for (auto ifa = ifaddrs; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) {
            continue;
        }
        if (ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if (ifa->ifa_flags & IFF_LOOPBACK) {
            continue;
        }
        auto sockaddr = (sockaddr_in*)ifa->ifa_addr;
        char buf[50];
        if (inet_ntop(AF_INET, &sockaddr->sin_addr, buf, sizeof(buf))) {
            address_list.emplace_back(buf);
        }
    }

    freeifaddrs(ifaddrs);
#endif

    return address_list;
}

std::string network_manager::get_default_address()
{
    return select_default_address(get_address_list());
}

std::string network_manager::select_default_address(const std::vector<std::string>& address_list)
{
    if (address_list.empty()) {
        return {};
    }

    auto is_private_address = [](const std::string& address) {
        constexpr uint32_t private_addr_list[] = {
            0x0a000000,
            0xac100000,
            0xc0a80000,
        };

        uint32_t addr;
        inet_pton(AF_INET, address.c_str(), &addr);
        addr = ntohl(addr);
        for (auto&& private_addr : private_addr_list) {
            if ((addr & private_addr) == private_addr) {
                return true;
            }
        }

        return false;
    };

    for (auto&& address : address_list) {
        if (is_private_address(address)) {
            return address;
        }
    }
    return address_list.front();
}

void network_manager::start_server(const std::string& host, uint16_t port, const audio_manager::capture_config& capture_config)
{
    _ioc = std::make_shared<asio::io_context>();
    {
        ip::tcp::endpoint endpoint { ip::make_address(host), port };

        ip::tcp::acceptor acceptor(*_ioc, endpoint.protocol());
        acceptor.set_option(ip::tcp::acceptor::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen();

        _audio_manager->start_loopback_recording(shared_from_this(), capture_config);
        asio::co_spawn(*_ioc, accept_tcp_loop(std::move(acceptor)), asio::detached);

        // start tcp success
        spdlog::info("tcp listen success on {}", endpoint);
    }

    {
        ip::udp::endpoint endpoint { ip::make_address(host), port };
        _udp_server = std::make_unique<udp_socket>(*_ioc, endpoint.protocol());
        _udp_server->bind(endpoint);
        asio::co_spawn(*_ioc, accept_udp_loop(), asio::detached);

        // start udp success
        spdlog::info("udp listen success on {}", endpoint);
    }

    _net_thread = std::thread([self = shared_from_this()] {
        self->_ioc->run();
    });

    spdlog::info("server started");
}

void network_manager::stop_server()
{
    if (_ioc) {
        _ioc->stop();
    }
    _net_thread.join();
    _audio_manager->stop();
    _playing_peer_list.clear();
    _udp_server = nullptr;
    _ioc = nullptr;
    spdlog::info("server stopped");
}

void network_manager::wait_server()
{
    _net_thread.join();
}

bool network_manager::is_running() const
{
    return _ioc != nullptr;
}

asio::awaitable<void> network_manager::read_loop(std::shared_ptr<tcp_socket> peer)
{
    while (true) {
        cmd_t cmd = cmd_t::cmd_none;
        auto [ec, _] = co_await asio::async_read(*peer, asio::buffer(&cmd, sizeof(cmd)));
        if (ec) {
            close_session(peer);
            spdlog::trace("{} {}", __func__, ec);
            break;
        }

        spdlog::trace("cmd {}", (uint32_t)cmd);

        if (cmd == cmd_t::cmd_get_format) {
            auto format = _audio_manager->get_format_binary();
            auto size = (uint32_t)format.size();
            std::array<asio::const_buffer, 3> buffers = {
                asio::buffer(&cmd, sizeof(cmd)),
                asio::buffer(&size, sizeof(size)),
                asio::buffer(format),
            };
            auto [ec, _] = co_await asio::async_write(*peer, buffers);
            if (ec) {
                close_session(peer);
                spdlog::trace("{} {}", __func__, ec);
                break;
            }
        } else if (cmd == cmd_t::cmd_start_play) {
            int id = add_playing_peer(peer);
            if (id <= 0) {
                spdlog::error("{} id error", __func__);
                close_session(peer);
                spdlog::trace("{} {}", __func__, ec);
                break;
            }
            std::array<asio::const_buffer, 2> buffers = {
                asio::buffer(&cmd, sizeof(cmd)),
                asio::buffer(&id, sizeof(id)),
            };
            auto [ec, _] = co_await asio::async_write(*peer, buffers);
            if (ec) {
                spdlog::trace("{} {}", __func__, ec);
                close_session(peer);
                break;
            }
            asio::co_spawn(*_ioc, heartbeat_loop(peer), asio::detached);
        } else if (cmd == cmd_t::cmd_heartbeat) {
            auto it = _playing_peer_list.find(peer);
            if (it != _playing_peer_list.end()) {
                it->second->last_tick = std::chrono::steady_clock::now();
            }
        } else {
            spdlog::error("{} error cmd", __func__);
            close_session(peer);
            break;
        }
    }
    spdlog::trace("stop {}", __func__);
}

asio::awaitable<void> network_manager::heartbeat_loop(std::shared_ptr<tcp_socket> peer)
{
    std::error_code ec;
    size_t _;

    steady_timer timer(*_ioc);
    while (true) {
        timer.expires_after(3s);
        std::tie(ec) = co_await timer.async_wait();
        if (ec) {
            break;
        }

        if (!peer->is_open()) {
            break;
        }

        auto it = _playing_peer_list.find(peer);
        if (it == _playing_peer_list.end()) {
            spdlog::trace("{} it == _playing_peer_list.end()", __func__);
            close_session(peer);
            break;
        }
        if (std::chrono::steady_clock::now() - it->second->last_tick > _heartbeat_timeout) {
            spdlog::info("{} timeout", it->first->remote_endpoint());
            close_session(peer);
            break;
        }

        auto cmd = cmd_t::cmd_heartbeat;
        std::tie(ec, _) = co_await asio::async_write(*peer, asio::buffer(&cmd, sizeof(cmd)));
        if (ec) {
            spdlog::trace("{} {}", __func__, ec);
            close_session(peer);
            break;
        }
    }
    spdlog::trace("stop {}", __func__);
}

asio::awaitable<void> network_manager::accept_tcp_loop(tcp_acceptor acceptor)
{
    while (true) {
        auto peer = std::make_shared<tcp_socket>(acceptor.get_executor());
        auto [ec] = co_await acceptor.async_accept(*peer);
        if (ec) {
            spdlog::error("{} {}", __func__, ec);
            co_return;
        }

        spdlog::info("accept {}", peer->remote_endpoint());

        // No-Delay
        peer->set_option(ip::tcp::no_delay(true), ec);
        if (ec) {
            spdlog::info("{} {}", __func__, ec);
        }

        asio::co_spawn(acceptor.get_executor(), read_loop(peer), asio::detached);
    }
}

asio::awaitable<void> network_manager::accept_udp_loop()
{
    while (true) {
        int id = 0;
        ip::udp::endpoint udp_peer;
        auto [ec, _] = co_await _udp_server->async_receive_from(asio::buffer(&id, sizeof(id)), udp_peer);
        if (ec) {
            spdlog::info("{} {}", __func__, ec);
            co_return;
        }

        fill_udp_peer(id, udp_peer);
    }
}

auto network_manager::close_session(std::shared_ptr<tcp_socket>& peer) -> playing_peer_list_t::iterator
{
    spdlog::info("close {}", peer->remote_endpoint());
    auto it = remove_playing_peer(peer);
    peer->shutdown(ip::tcp::socket::shutdown_both);
    peer->close();
    return it;
}

int network_manager::add_playing_peer(std::shared_ptr<tcp_socket>& peer)
{
    if (_playing_peer_list.contains(peer)) {
        spdlog::error("{} repeat add tcp://{}", __func__, peer->remote_endpoint());
        return 0;
    }

    auto info = _playing_peer_list[peer] = std::make_shared<peer_info_t>();
    static int g_id = 0;
    info->id = ++g_id;
    info->last_tick = std::chrono::steady_clock::now();

    spdlog::trace("{} add id:{} tcp://{}", __func__, info->id, peer->remote_endpoint());
    return info->id;
}

auto network_manager::remove_playing_peer(std::shared_ptr<tcp_socket>& peer) -> playing_peer_list_t::iterator
{
    auto it = _playing_peer_list.find(peer);
    if (it == _playing_peer_list.end()) {
        spdlog::error("{} repeat remove tcp://{}", __func__, peer->remote_endpoint());
        return it;
    }

    it = _playing_peer_list.erase(it);
    spdlog::trace("{} remove tcp://{}", __func__, peer->remote_endpoint());
    return it;
}

void network_manager::fill_udp_peer(int id, asio::ip::udp::endpoint udp_peer)
{
    auto it = std::find_if(_playing_peer_list.begin(), _playing_peer_list.end(), [id](const playing_peer_list_t::value_type& e) {
        return e.second->id == id;
    });

    if (it == _playing_peer_list.cend()) {
        spdlog::error("{} no tcp peer id:{} udp://{}", __func__, id, udp_peer);
        return;
    }

    it->second->udp_peer = udp_peer;
    spdlog::info("{} fill udp peer id:{} tcp://{} udp://{}", __func__, id, it->first->remote_endpoint(), udp_peer);
}

void network_manager::broadcast_audio_data(const char* data, size_t count, int block_align)
{
    if (count <= 0) {
        return;
    }
    // spdlog::trace("broadcast_audio_data count: {}", count);

    // divide udp frame
    constexpr int mtu = 1492;
    int max_seg_size = mtu - 20 - 8;
    max_seg_size -= max_seg_size % block_align; // one single sample can't be divided

    std::list<std::shared_ptr<std::vector<uint8_t>>> seg_list;

    for (int begin_pos = 0; begin_pos < count;) {
        const int real_seg_size = std::min((int)count - begin_pos, max_seg_size);
        auto seg = std::make_shared<std::vector<uint8_t>>(real_seg_size);
        std::copy((const uint8_t*)data + begin_pos, (const uint8_t*)data + begin_pos + real_seg_size, seg->begin());
        seg_list.push_back(seg);
        begin_pos += real_seg_size;
    }

    _ioc->post([seg_list = std::move(seg_list), self = shared_from_this()] {
        for (const auto& seg : seg_list) {
            for (auto& [peer, info] : self->_playing_peer_list) {
                self->_udp_server->async_send_to(asio::buffer(*seg), info->udp_peer, [seg](const asio::error_code& ec, std::size_t bytes_transferred) { });
            }
        }
    });
}


void network_manager::start_client(const std::string& host, uint16_t port)
{
    if (_ioc == nullptr) {
        _ioc = std::make_shared<asio::io_context>();
    }

    auto task = [] (std::shared_ptr<network_manager> self, const std::string host, uint16_t port) -> void {
        assert(self != nullptr && "self is a null pointer");
        assert(self->_ioc != nullptr && "network_manager::_ioc is a null pointer");

        try {
            spdlog::info("connect to server {}:{}", host, port);
            asio::co_spawn(*self->_ioc, self->client_connect(self, host, port), asio::detached);
            auto work = asio::make_work_guard(*self->_ioc);
            self->_ioc->run();
        } catch (std::exception& e) {
            spdlog::error("exception in io_context thread: {}", e.what());
        }
    };

    try {
        if (!_net_thread.joinable()) {
            spdlog::info("start thread");
            _net_thread = std::thread(task, shared_from_this(), host, port);
        }
    } catch (std::exception& e) {
        spdlog::error("failed to start the network thread: {}", e.what());
    }

    spdlog::info("start client");
}

asio::awaitable<void> network_manager::client_heartbeat_loop(std::shared_ptr<tcp_socket> socket)
{
    asio::steady_timer timer(*_ioc);

    while (true) {
        if (!is_running()) {
            co_return;
        }
        cmd_t cmd = cmd_t::cmd_heartbeat;
        auto [ec, _] = co_await asio::async_write(*socket, asio::buffer(&cmd, sizeof(cmd)));
        if (ec) {
            spdlog::error("send cmd_heartbeat failed, {}", ec.message());
            co_return;
        }

        spdlog::trace("send cmd_heartbeat successfully, {}", ec.message());
        timer.expires_after(std::chrono::seconds(3));
        co_await timer.async_wait(asio::use_awaitable);
    }
}

asio::awaitable<void> network_manager::client_udp_loop(audio_manager::AudioFormat audio_format, const std::string host, uint16_t port, uint32_t id)
{
    spdlog::info("udp connect: {}:{}, id:{}", host, port, id);
    
    asio::steady_timer timer(*_ioc);
    ip::udp::resolver resolver(*_ioc);
    ip::udp::endpoint endpoint = *resolver.resolve(asio::ip::udp::v4(), host, std::to_string(port)).begin();
    ip::udp::socket socket(*_ioc, asio::ip::udp::v4());

    asio::error_code ec{};
    uint32_t n;
    co_await socket.async_connect(endpoint, asio::redirect_error(asio::use_awaitable, ec));

    n = co_await socket.async_send(asio::buffer(&id, sizeof(id)), asio::redirect_error(asio::use_awaitable, ec));
    if (ec) {
        spdlog::error("udp send id failed, {}", ec.message());
    }
    spdlog::info("send size: {}, content: {}", n, std::format("{:08x}", id));

    std::array<char, 4096> recv_buffer {};
    _audio_manager->audio_init(audio_format);
    _audio_manager->audio_start();
    while (true) {
        if (!is_running()) {
            co_return;
        }
        n = co_await socket.async_receive(asio::buffer(recv_buffer), asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            continue;
        }
        _audio_manager->audio_play(std::vector<char>(recv_buffer.begin(), recv_buffer.begin() + n));
    }
}

asio::awaitable<void> network_manager::client_connect(std::shared_ptr<network_manager> self, const std::string host, uint16_t port)
{
    audio_manager::AudioFormat audio_format;
    uint32_t udp_id = 0;
    auto socket = std::make_shared<tcp_socket>(*self->_ioc);
    ip::tcp::resolver resolver(*self->_ioc);
    ip::tcp::resolver::results_type endpoints = resolver.resolve(host, std::to_string(port));

    try {
        // resolve
        {
            auto [ec, _] = co_await asio::async_connect(*socket, endpoints);
            if (ec) {
                spdlog::error("error connecting to server: {}", ec.message());
                co_return;
            }
        }

        // get audio format
        {
            cmd_t cmd = cmd_t::cmd_get_format;
            uint32_t size = 0;
            auto [ec, _] = co_await asio::async_write(*socket, asio::buffer(&cmd, sizeof(cmd)));
            if (ec) {
                spdlog::error("send cmd_get_format error, {}", ec.message());
                co_return;
            }
            spdlog::info("send cmd_get_format successfully, cmd: {}", (size_t)cmd);

            cmd = cmd_t::cmd_none;
            std::array<uint32_t, 2> buffer = { static_cast<uint32_t>(cmd), size };
            std::tie(ec, _) = co_await asio::async_read(*socket, asio::buffer(buffer.data(), sizeof(buffer)));
            if (ec) {
                spdlog::error("read cmd_get_format error, {}", ec.message());
                co_return;
            }
            cmd = static_cast<cmd_t>(buffer[0]);
            size = buffer[1];
            if (cmd != cmd_t::cmd_get_format || size <= 0) {
                spdlog::error("read cmd_get_format error, cmd: {}, size: {}", (size_t)cmd, size);
                co_return;
            }
            spdlog::info("read cmd_get_format successfully, cmd: {}, size: {}", (size_t)cmd, size);

            std::vector<char> format(size);
            std::tie(ec, _) = co_await asio::async_read(*socket, asio::buffer(format, size));
            if (ec) {
                spdlog::error("error read audio format, {}", ec.message());
                co_return;
            }
            std::string str(format.begin(), format.end());
            if (!audio_format.ParseFromString(str)) {
                spdlog::error("error parse audio format");
                co_return;
            }
            spdlog::info("get audio format successfully, sample_rate: {}, channels: {}, encoding: {}",
                         (uint32_t)audio_format.sample_rate(), (uint32_t)audio_format.channels(), (uint32_t)audio_format.encoding());
        }

        // start play
        {
            cmd_t cmd = cmd_t::cmd_start_play;
            auto [ec, _] = co_await asio::async_write(*socket, asio::buffer(&cmd, sizeof(cmd)));
            if (ec) {
                spdlog::error("error send cmd_start_play, {}", ec.message());
                co_return;
            }

            cmd = cmd_t::cmd_none;
            std::array<uint32_t, 2> buffer = { static_cast<uint32_t>(cmd), udp_id};
            std::tie(ec, _) = co_await asio::async_read(*socket, asio::buffer(buffer.data(), sizeof(buffer)));
            if (ec) {
                spdlog::error("error read cmd_start_play. {}", ec.message());
                co_return;
            }
            cmd = static_cast<cmd_t>(buffer[0]);
            if (cmd != cmd_t::cmd_start_play) {
                spdlog::error("read cmd_start_play error, cmd: {}, udp_id: {}", (size_t)cmd, udp_id);
                co_return;
            }

            udp_id = buffer[1];
            spdlog::info("get udp_id successfully, udp_id: {}", std::format("{:08x}", udp_id));
        }

        asio::co_spawn(*self->_ioc, self->client_heartbeat_loop(socket), asio::detached);
        asio::co_spawn(*self->_ioc, self->client_udp_loop(audio_format, host, port, udp_id), asio::detached);
    } catch (std::exception& e) {
        spdlog::error("error connecting to server: {}", e.what());
    }
}

void network_manager::wait_client()
{
    _net_thread.join();
}

void network_manager::stop_client()
{
    if (_ioc) {
        _ioc->stop();
    }
    _net_thread.join();
    _ioc = nullptr;
    spdlog::info("client stopped");
}
