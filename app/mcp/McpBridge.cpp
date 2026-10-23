#include "McpEndpoint.hpp"
#include "McpProtocol.hpp"
#include <QCoreApplication>
#include <QLocalSocket>
#include <QTimer>
#include <QElapsedTimer>
#include <memory>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char **argv) {
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::ios::sync_with_stdio(false);
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.contains("--help")) {
        std::cerr << "vlt_mcp [--endpoint NAME]\nSet VLTONE_MCP_TOKEN and enable External agents / MCP in VLTONE.\n";
        return 0;
    }
    QString endpoint = vlt::mcp::endpointName();
    const int at = args.indexOf("--endpoint");
    if (at >= 0) {
        if (at + 1 == args.size()) return 2;
        endpoint = args.at(at + 1);
    }
    const auto token = qgetenv("VLTONE_MCP_TOKEN");
    if (token.isEmpty()) { std::cerr << "VLTONE_MCP_TOKEN is missing. Copy the connection configuration from VLTONE.\n"; return 2; }
    QLocalSocket socket;
    socket.setReadBufferSize(vlt::mcp::maxMessageBytes + 1);
    socket.connectToServer(endpoint);
    if (!socket.waitForConnected(3000)) {
        std::cerr << "VLTONE MCP is unavailable. Start VLTONE and enable External agents / MCP.\n";
        return 3;
    }
    const auto auth = vlt::mcp::Json{{"vltMcp", 1}, {"token", token.toStdString()}}.dump() + '\n';
    socket.write(auth.data(), qint64(auth.size()));
    socket.flush();
    QElapsedTimer authentication; authentication.start();
    while (!socket.canReadLine() && authentication.elapsed() < 3000)
        if (!socket.waitForReadyRead(int(3000-authentication.elapsed()))) break;
    if (!socket.canReadLine()) { std::cerr << "VLTONE MCP authentication timed out.\n"; return 4; }
    const auto answer = vlt::mcp::Json::parse(socket.readLine().toStdString(), nullptr, false);
    if (!answer.is_object() || answer.value("authenticated", false) != true) {
        std::cerr << "VLTONE MCP authentication failed. Copy a new connection configuration.\n";
        return 4;
    }
    auto pending = std::make_shared<std::atomic<std::size_t>>(0);
    QObject::connect(&socket, &QLocalSocket::readyRead, &app, [&] {
        const auto bytes = socket.readAll();
        std::cout.write(bytes.constData(), bytes.size());
        std::cout.flush();
        if (!std::cout.good()) app.exit(5);
    });
    QObject::connect(&socket, &QLocalSocket::disconnected, &app, [&] { app.exit(3); });
    // Windows stdin is a pipe handle, not a socket usable by QSocketNotifier.
    // Bound queued bytes before posting to the Qt thread, including slow peers.
    std::thread([&app, &socket, pending] {
        std::string line;
        char ch;
        while (std::cin.get(ch)) {
            if (ch == '\n') {
                line += '\n';
                const auto size = line.size();
                if (pending->fetch_add(size) + size > 2 * vlt::mcp::maxMessageBytes) {
                    QMetaObject::invokeMethod(&app, [&app] { app.exit(6); }, Qt::QueuedConnection);
                    return;
                }
                QMetaObject::invokeMethod(&app, [&app, &socket, pending, line = std::move(line), size] {
                    pending->fetch_sub(size);
                    if (socket.bytesToWrite() > qint64(2 * vlt::mcp::maxMessageBytes)) { app.exit(6); return; }
                    socket.write(line.data(), qint64(line.size()));
                }, Qt::QueuedConnection);
                line.clear();
            } else {
                line += ch;
                if (line.size() > vlt::mcp::maxMessageBytes) {
                    QMetaObject::invokeMethod(&app, [&app] { app.exit(6); }, Qt::QueuedConnection);
                    return;
                }
            }
        }
        QMetaObject::invokeMethod(&app, [&app, &socket] { socket.abort(); app.quit(); }, Qt::QueuedConnection);
    }).detach();
    const int result = app.exec();
    socket.abort();
    std::cout.flush();
    // The only worker may still be blocked on the client's stdin. Do not tear
    // down its Qt dispatch target before terminating this dedicated relay.
    std::_Exit(result);
}
