#include "AiChatChecks.hpp"
#include "LlmClient.hpp"
#include "BrowserPrefs.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QSettings>
#include <QTemporaryDir>
#include <QStandardPaths>

#include <cstdio>

namespace ui {
bool checkAiTransport() {
    int failures = 0;
    const auto check = [&failures](bool ok, const char* name) {
        std::fprintf(stderr, "%s AI transport: %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };
    {
        QSettings settings;
        const QStringList keys{"browser/folders", "browser/aiFolders", "browser/collections"};
        QList<QVariant> saved;
        for (const auto& key : keys) { saved << settings.value(key); settings.remove(key); }
        check(browserprefs::aiContentPaths().empty(), "default home folders do not grant library access");
        QTemporaryDir library;
        browserprefs::addFolder(library.path());
        check(browserprefs::aiContentPaths() == QStringList{library.path()}, "adding one folder never grants the default home roots");
        browserprefs::removeFolder(library.path());
        check(browserprefs::aiContentPaths().empty(), "removing the folder revokes its grant");
        const auto collection = browserprefs::createCollection("AI test samples");
        browserprefs::addToCollection(collection, library.path());
        check(browserprefs::aiContentPaths().contains(library.path()), "explicit collection entries are available to AI");
        for (int i = 0; i < keys.size(); ++i) {
            if (saved[i].isValid()) settings.setValue(keys[i], saved[i]);
            else settings.remove(keys[i]);
        }
    }
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost)) return false;
    enum Scenario { BusyThenSuccess, EmptyThenSuccess, PartialDisconnect, TruncatedStream, StreamBusy, AlwaysBusy };
    Scenario scenario = BusyThenSuccess;
    int requests = 0;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (auto* socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::readyRead, socket,
                [&, socket, buffer = QByteArray(), sent = false]() mutable {
                buffer += socket->readAll();
                const auto boundary = buffer.indexOf("\r\n\r\n");
                if (sent || boundary < 0) return;
                int size = 0;
                for (const auto& line : buffer.left(boundary).split('\n'))
                    if (line.toLower().startsWith("content-length:")) size = line.mid(15).trimmed().toInt();
                if (buffer.size() < boundary + 4 + size) return;
                sent = true;
                ++requests;
                QByteArray status = "200 OK", type = "application/json";
                QByteArray body = R"({"choices":[{"message":{"content":"Ready"},"finish_reason":"stop"}]})";
                if (scenario == EmptyThenSuccess && requests == 1) {
                    body = R"({"choices":[{"message":{"content":""},"finish_reason":"stop"}]})";
                } else if (scenario == AlwaysBusy || (scenario == BusyThenSuccess && requests == 1)) {
                    status = "503 Service Unavailable";
                    body = R"({"error":{"message":"temporarily busy"}})";
                } else if (scenario == PartialDisconnect || scenario == TruncatedStream) {
                    type = "text/event-stream";
                    body = "data: {\"choices\":[{\"delta\":{\"content\":\"Saved fragment\"}}]}\r\n\r\n";
                } else if (scenario == StreamBusy && requests == 1) {
                    type = "text/event-stream";
                    body = "data: {\"error\":{\"code\":429,\"message\":\"busy\"}}\n\n";
                }
                const int extra = scenario == PartialDisconnect ? 80 : 0;
                socket->write("HTTP/1.1 " + status + "\r\nContent-Type: " + type +
                    "\r\nContent-Length: " + QByteArray::number(body.size() + extra) +
                    "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        }
    });
    LlmClient client(LlmClient::Provider::OpenAi);
    LlmConfig config;
    config.transport = LlmConfig::Transport::Direct;
    config.endpoint = QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort());
    config.model = "local-test";
    config.maxRetries = 1;
    client.setConfig(config);
    client.setAvailableTools({});
    const auto run = [&] {
        requests = 0;
        daw::ai::ModelReply answer;
        bool finished = false;
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        client.send("Test", {{daw::ai::Role::User, "Hello", {}, {}}}, [&](auto reply) {
            answer = std::move(reply); finished = true; loop.quit();
        });
        deadline.start(12000);
        if (!finished) loop.exec();
        check(finished && !client.busy(), "request reaches a terminal state");
        if (!finished) client.cancel();
        return answer;
    };
    auto answer = run();
    check(requests == 2 && answer.error.empty() && answer.text == "Ready", "503 retries and succeeds");
    scenario = EmptyThenSuccess;
    answer = run();
    check(requests == 2 && answer.error.empty(), "an empty response gets a bounded retry");
    scenario = StreamBusy;
    answer = run();
    check(requests == 2 && answer.error.empty(), "SSE 429 retries before output");
    scenario = PartialDisconnect;
    answer = run();
    check(requests == 1 && !answer.error.empty() && answer.text == "Saved fragment" && answer.calls.empty(),
          "disconnect keeps text without repeating the request");
    scenario = TruncatedStream;
    answer = run();
    check(requests == 1 && !answer.error.empty() && answer.text == "Saved fragment",
          "missing completion marker is not treated as success");
    scenario = AlwaysBusy;
    requests = 0;
    bool callback = false, waiting = false;
    QEventLoop cancellation;
    client.setStatusSink([&](const QString& status) {
        if (status.contains("1/1")) {
            waiting = client.busy();
            QTimer::singleShot(0, &cancellation, [&] { client.cancel(); cancellation.quit(); });
        }
    });
    client.send("Test", {{daw::ai::Role::User, "Hello", {}, {}}}, [&](auto) { callback = true; });
    QTimer::singleShot(3000, &cancellation, &QEventLoop::quit);
    cancellation.exec();
    check(waiting && !client.busy() && !callback && requests == 1, "Stop cancels backoff and suppresses the callback");
    client.setStatusSink({});
    client.cancel();
    return failures == 0;
}
}
