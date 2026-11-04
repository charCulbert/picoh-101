// Wi-Fi: serves the GUI page, then one GUI line per WebSocket message both ways. All in the control loop
// (lwIP's poll mode), so it never races the USB or DIN code.
#include "board.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
extern "C"
{
#include "dhcpserver.h"
}
#include "lwip/apps/mdns.h"
#include "lwip/tcp.h"
#include "pico/cyw43_arch.h"
#include "gui_protocol.h"
#include "websocket.h"
#include "web_files.generated.h"

namespace
{
constexpr const char* hostName = "picoh-101";
constexpr int maxClients = 8; // a browser loads the page over up to 6 connections at once

struct Client
{
    tcp_pcb* pcb = nullptr;
    bool webSocket = false;
    uint8_t in[1024];
    size_t inLength = 0;
    const uint8_t* sending = nullptr; // rest of a file being served
    size_t sendingLeft = 0;
    Gui gui { EVENT_PORT_WEB };
};

Client clients[maxClients];
Guis* guis;
bool stationMode, joined;
dhcp_server_t dhcpServer;

// Lets go of the client's notes, then closes it. True if lwIP had to abort the pcb: a callback that called
// this must then return ERR_ABRT, as lwIP no longer has the pcb.
bool drop (Client& c)
{
    tcp_pcb* pcb = c.pcb;
    c.webSocket = false; // so the release's lines don't go to it
    guis->release (c.gui);
    bool aborted = false;
    if (pcb)
    {
        tcp_arg (pcb, nullptr);
        tcp_recv (pcb, nullptr);
        tcp_sent (pcb, nullptr);
        tcp_err (pcb, nullptr);
        if (tcp_close (pcb) != ERR_OK)
        {
            tcp_abort (pcb);
            aborted = true;
        }
    }
    c = Client {};
    return aborted;
}

bool sendFrame (Client& c, const char* text)
{
    size_t length = strlen (text);
    uint8_t header[4];
    size_t headerLength = ws::frameHeader (ws::text, length, header);
    if (tcp_sndbuf (c.pcb) < headerLength + length)
        return false;  // a slow GUI misses this one
    tcp_write (c.pcb, header, uint16_t (headerLength), TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE);
    tcp_write (c.pcb, text, uint16_t (length), TCP_WRITE_FLAG_COPY);
    tcp_output (c.pcb);
    return true;
}

// Sends as much of the file as fits; tcp_sent calls back for the rest.
void continueFile (Client& c)
{
    while (c.sendingLeft)
    {
        uint16_t room = tcp_sndbuf (c.pcb);
        if (room == 0)
            break;
        uint16_t n = uint16_t (c.sendingLeft < room ? c.sendingLeft : room);
        if (tcp_write (c.pcb, c.sending, n, 0) != ERR_OK)
            break; // flash data, no copy
        c.sending += n;
        c.sendingLeft -= n;
    }
    tcp_output (c.pcb);
}

// False if the client was dropped and its pcb aborted.
bool readWebSocket (Client& c)
{
    ws::Frame frame;
    size_t used = 0;
    while (ws::readFrame (c.in + used, c.inLength - used, frame))
    {
        used += frame.size;
        if (frame.opcode == ws::close)
            return ! drop (c);
        if (frame.opcode == ws::text && frame.length < 64)
        {
            char text[64];
            memcpy (text, frame.payload, frame.length);
            text[frame.length] = 0;
            if (strcmp (text, "hello") == 0)
                guis->sendState ([&] (const char* line) { sendFrame (c, line); });
            else
                guis->receive (c.gui, text);
        }
    }
    memmove (c.in, c.in + used, c.inLength - used);
    c.inLength -= used;
    return true;
}

const char* header (const char* request, const char* name, size_t& length)
{
    for (const char* at = strstr (request, "\r\n"); at; at = strstr (at + 2, "\r\n"))
        if (strncasecmp (at + 2, name, strlen (name)) == 0)
        {
            const char* value = at + 2 + strlen (name);
            while (*value == ' ')
                ++value;
            length = strcspn (value, "\r\n");
            return value;
        }
    return nullptr;
}

void readHttp (Client& c)
{
    c.in[c.inLength < sizeof c.in ? c.inLength : sizeof c.in - 1] = 0;
    char* request = reinterpret_cast<char*> (c.in);
    if (! strstr (request, "\r\n\r\n"))
        return; // headers not all here yet

    char path[64] = "";
    sscanf (request, "GET %63s", path);
    size_t keyLength;
    const char* key = header (request, "Sec-WebSocket-Key:", keyLength);

    if (strcmp (path, "/ws") == 0 && key)
    {
        char accept[29], reply[160];
        ws::acceptKey (key, keyLength, accept);
        int n = snprintf (reply, sizeof reply,
                          "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                          "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",
                          accept);
        tcp_write (c.pcb, reply, uint16_t (n), TCP_WRITE_FLAG_COPY);
        c.webSocket = true;
        c.inLength = 0;
        guis->sendState ([&] (const char* line) { sendFrame (c, line); });
        return;
    }

    if (strcmp (path, "/") == 0)
        strcpy (path, "/index.html");
    for (const WebFile& f : webFiles)
        if (strcmp (path, f.path) == 0)
        {
            char reply[160];
            int n = snprintf (reply, sizeof reply,
                              "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                              "Cache-Control: no-cache\r\nConnection: close\r\n\r\n",
                              f.type, unsigned (f.size));
            tcp_write (c.pcb, reply, uint16_t (n), TCP_WRITE_FLAG_COPY);
            c.sending = f.data;
            c.sendingLeft = f.size;
            c.inLength = 0;
            continueFile (c);
            return;
        }
    static const char notFound[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    tcp_write (c.pcb, notFound, sizeof notFound - 1, 0);
    tcp_output (c.pcb);
    c.sending = reinterpret_cast<const uint8_t*> (notFound); // so sent() closes it
    c.inLength = 0;
}

err_t received (void* arg, tcp_pcb* pcb, pbuf* p, err_t)
{
    Client& c = *static_cast<Client*> (arg);
    if (! p)
        return drop (c) ? ERR_ABRT : ERR_OK;
    size_t room = sizeof c.in - 1 - c.inLength;
    size_t n = p->tot_len < room ? p->tot_len : room;
    pbuf_copy_partial (p, c.in + c.inLength, uint16_t (n), 0);
    c.inLength += n;
    tcp_recved (pcb, p->tot_len);
    pbuf_free (p);
    if (c.webSocket)
        return readWebSocket (c) ? ERR_OK : ERR_ABRT;
    readHttp (c);
    return ERR_OK;
}

err_t sent (void* arg, tcp_pcb*, u16_t)
{
    Client& c = *static_cast<Client*> (arg);
    if (c.sendingLeft)
        continueFile (c);
    else if (! c.webSocket && c.sending && drop (c)) // file done: Connection: close
        return ERR_ABRT;
    return ERR_OK;
}

void failed (void* arg, err_t)
{
    Client& c = *static_cast<Client*> (arg);
    // lwIP has already freed the pcb: forget it before the release sends lines to the GUIs.
    c.pcb = nullptr;
    c.webSocket = false;
    guis->release (c.gui);
    c = Client {};
}

err_t accepted (void*, tcp_pcb* pcb, err_t err)
{
    if (err != ERR_OK || ! pcb)
        return ERR_VAL;
    for (Client& c : clients)
        if (! c.pcb)
        {
            c = Client {};
            c.pcb = pcb;
            tcp_arg (pcb, &c);
            tcp_recv (pcb, received);
            tcp_sent (pcb, sent);
            tcp_err (pcb, failed);
            tcp_nagle_disable (pcb); // knob moves are tiny; send them now
            return ERR_OK;
        }
    tcp_abort (pcb);
    return ERR_ABRT;
}

void listen()
{
    tcp_pcb* pcb = tcp_new_ip_type (IPADDR_TYPE_ANY);
    tcp_bind (pcb, IP_ANY_TYPE, 80);
    pcb = tcp_listen_with_backlog (pcb, maxClients);
    tcp_accept (pcb, accepted);
}

void announce()
{
    netif* nif = &cyw43_state.netif[stationMode ? CYW43_ITF_STA : CYW43_ITF_AP];
    if (stationMode)
    {
        mdns_resp_init();
        mdns_resp_add_netif (nif, hostName);
        mdns_resp_add_service (nif, hostName, "_http", DNSSD_PROTO_TCP, 80, nullptr, nullptr);
        usb::log ("web: http://%s/ or http://%s.local/", ip4addr_ntoa (netif_ip4_addr (nif)), hostName);
    }
    else
        usb::log ("web: http://%s/", ip4addr_ntoa (netif_ip4_addr (nif)));
}
} // namespace

namespace web
{
bool start (Guis& p)
{
    guis = &p;
    if (cyw43_arch_init())
    {
        usb::log ("web: the radio didn't start");
        return false;
    }
#ifdef WIFI_SSID
    stationMode = true;
    cyw43_arch_enable_sta_mode();
    cyw43_arch_wifi_connect_async (WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
    usb::log ("web: joining %s", WIFI_SSID);
#else
    stationMode = false;
    cyw43_arch_enable_ap_mode (hostName, AP_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
    ip_addr_t address, mask;
    IP4_ADDR (ip_2_ip4 (&address), 192, 168, 4, 1);
    IP4_ADDR (ip_2_ip4 (&mask), 255, 255, 255, 0);
    dhcp_server_init (&dhcpServer, &address, &mask);
    usb::log ("web: network %s, password %s", hostName, AP_PASSWORD);
    joined = true;
    announce();
#endif
    listen();
    return true;
}

void poll()
{
    cyw43_arch_poll();
#ifdef WIFI_SSID
    if (! joined && stationMode)
    {
        int status = cyw43_tcpip_link_status (&cyw43_state, CYW43_ITF_STA);
        if (status == CYW43_LINK_UP)
        {
            joined = true;
            announce();
        }
        else if (status < 0)
        {
            usb::log ("web: couldn't join (%d), trying again", status);
            cyw43_arch_wifi_connect_async (WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
        }
    }
#endif
}

void sendText (const char* line, const Gui* skip)
{
    for (Client& c : clients)
        if (c.pcb && c.webSocket && &c.gui != skip)
            sendFrame (c, line);
}
} // namespace web
