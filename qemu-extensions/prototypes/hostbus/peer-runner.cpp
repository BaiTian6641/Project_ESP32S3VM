// SPDX-License-Identifier: MIT
// Test-only studio-side process. It links Qt/PeripheralTransport, not QEMU.
#include "PeripheralTransport.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSocketNotifier>
#include <QTextStream>
#include <QTimer>
#include <QUuid>
#include <cstdio>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

static void record(const QJsonObject &message)
{
    const auto bytes = QJsonDocument(message).toJson(QJsonDocument::Compact);
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    auto option = [&](const QString &name, const QString &fallback) {
        const int index = arguments.indexOf(name);
        return index >= 0 && index + 1 < arguments.size() ? arguments.at(index + 1) : fallback;
    };
    bool valid = false;
    const int delayMs = option("--delay-ms", "100").toInt(&valid);
    if (!valid || delayMs < 0 || delayMs > 600000) return 2;
    const quint64 latencyNs = option("--latency-ns", "0").toULongLong(&valid);
    if (!valid || latencyNs > 1000000000ULL) return 2;
    const bool dropReplies = arguments.contains("--drop-replies");
    const QString session = QUuid::createUuid().toString(QUuid::WithoutBraces);
    PeripheralTransport transport;
    PeripheralTransportLimits limits;
    limits.hostWatchdogMs = 600000; // QEMU's independent realtime watchdog is under test.
    if (!transport.setLimits(limits) || !transport.beginSession(session) || !transport.listen()) return 3;
    record({{"event", "listening"}, {"port", transport.port()}, {"session_id", session}});
    QObject::connect(&transport, &PeripheralTransport::ready, &app, [&](const QStringList &features) {
        record({{"event", "ready"}, {"connection_id", transport.connectionId()}, {"capabilities", QJsonArray::fromStringList(features)}});
    });
    QObject::connect(&transport, &PeripheralTransport::protocolError, &app, [](const QString &error) {
        record({{"event", "protocol-error"}, {"reason", error}});
    });
    QObject::connect(&transport, &PeripheralTransport::requestReceived, &app, [&](const BusEnvelope &request) {
        record({{"event", "request"}, {"ordinal", request.requestId}, {"virtual_ns", QString::number(request.virtualTimeNs)},
                {"epoch", QString::number(request.resetEpoch)}, {"connection_id", request.connectionId}});
        if (dropReplies) return;
        // Full original context survives the delay; stale callbacks are rejected.
        QTimer::singleShot(delayMs, &transport, [&, request]() {
            QJsonObject data;
            for (const QString &key : {QStringLiteral("bus"), QStringLiteral("controller_id"),
                    QStringLiteral("endpoint_ids"), QStringLiteral("net_ids")}) data[key] = request.data.value(key);
            data["phase_statuses"] = QJsonArray{QJsonObject{{"index", 0}, {"status", "ack"}}};
            data["modeled_latency_ns"] = QString::number(latencyNs);
            data["accepted_length"] = 0;
            data["payload_encoding"] = "base64";
            data["payload"] = "Wg==";
            data["error_message"] = "";
            QString error;
            const bool accepted = transport.sendResponse(request, data, BusStatus::Ok, request.virtualTimeNs + latencyNs, &error);
            record({{"event", "response"}, {"ordinal", request.requestId}, {"accepted", accepted},
                    {"virtual_ns", QString::number(request.virtualTimeNs + latencyNs)}, {"reason", error}});
        });
    });
    QByteArray input;
    const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    QSocketNotifier notifier(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(&notifier, &QSocketNotifier::activated, &app, [&](QSocketDescriptor, QSocketNotifier::Type) {
        char bytes[4096];
        for (;;) {
            const ssize_t count = ::read(STDIN_FILENO, bytes, sizeof(bytes));
            if (count <= 0) { if (count == 0) notifier.setEnabled(false); break; }
            input.append(bytes, qsizetype(count));
            if (input.size() > 8192) { record({{"event", "control-error"}, {"reason", "Control line limit exceeded"}}); input.clear(); break; }
        }
        while (input.contains('\n')) {
            const int end = input.indexOf('\n');
            const auto command = QJsonDocument::fromJson(input.left(end)).object();
            input.remove(0, end + 1);
            QString error;
            bool accepted = false;
            if (command.value("op") == "generation") {
                bool epochValid, topologyValid, timeValid;
                const auto epoch = command.value("epoch").toString().toULongLong(&epochValid);
                const auto topology = command.value("topology").toString().toULongLong(&topologyValid);
                const auto time = command.value("virtual_ns").toString().toULongLong(&timeValid);
                accepted = epochValid && topologyValid && timeValid && transport.resetGeneration(epoch, topology, time, &error);
            } else if (command.value("op") == "quit") {
                accepted = true; app.quit();
            }
            record({{"event", "control-ack"}, {"accepted", accepted}, {"reason", error}});
        }
    });
    return app.exec();
}
