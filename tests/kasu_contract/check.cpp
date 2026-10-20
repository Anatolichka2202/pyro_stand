#include "integration.h"
#include "bus/kasu_event_bus.h"
#include "debug/bcvm_debug_module.h"
#include "protocol/bcvm_frames.h"
#include "runtime/bcvm_udp_exchange.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <cstdio>
#include <functional>
using namespace kasu;
static bool wait(const std::function<bool()> &ready, int timeoutMs = 3000) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents();
        if (ready())
            return true;
        QThread::msleep(1);
    }
    return false;
}
// Проверяем настоящий издатель КАСУПП и потребитель стенда без обращения к оборудованию.
class IntegrationTest {
  public:
    static int run() {
        QTemporaryDir temp;
        KasuEventBus bus;
        BcvmUdpExchange exchange(bcvm::confirmedBcvmWireProfile(), 1000);
        BcvmDebugModule debug(&bus, &exchange, temp.filePath("exchange.jsonl"));
        QString error;
        const auto socket = temp.filePath("contract.sock");
        if (!debug.listenForStand(socket, &error)) {
            fprintf(stderr, "listen: %s\n", error.toUtf8().constData());
            return 1;
        }
        IntegrationSession::Options options;
        options.debugSocket = socket;
        options.bindIp = QHostAddress::LocalHost;
        options.boardIp = QHostAddress::LocalHost;
        options.telemetryPort = 0;
        options.firstStageYps = 1;
        options.kpYps = 1;
        IntegrationSession modern(options);
        options.boardTransport = IntegrationSession::BoardTransport::LegacySerial;
        options.serialPort = "NONEXISTENT_CONTRACT_COM";
        IntegrationSession old(options);
        modern.start();
        old.start();
        if (!wait([&] { return modern.debugConnected() && old.debugConnected(); }))
            return 2;
        KasuEvent prepared;
        prepared.kind = KasuEventKind::CyclogramPrepared;
        prepared.cyclogramBytes =
            "START_UTC_DATE = 2026-10-14\nSTART_UTC_TIME = "
            "12:00:00\nOTHER1=0\nOTHER2=1\nIGNITE_PYRO_CANDLES_ENGINES_9_TO_12=-20\nLIFT_OFF_CONTACT=0\n";
        bus.publish(prepared);
        if (!wait([&] { return modern.hasPlan() && old.hasPlan(); }))
            return 3;
        if (modern.uploadId() != old.uploadId() ||
            modern.cyclogramSha256() != old.cyclogramSha256())
            return 4;
        KasuEvent uploaded;
        uploaded.kind = KasuEventKind::CyclogramUploadResolved;
        uploaded.confirmed = true;
        uploaded.value = "CYCLOGRAM_UPLOAD_COMPLETE";
        uploaded.bcvmPacket = 42;
        uploaded.boardDate = "20261014";
        uploaded.boardTime = "00043200";
        bus.publish(uploaded);
        if (!wait([&] {
                return modern.uploadState() == "CONFIRMED" && old.uploadState() == "CONFIRMED";
            }))
            return 5;
        KasuEvent unlock;
        unlock.kind = KasuEventKind::BcvmLockConfirmed;
        unlock.locked = false;
        unlock.bcvmPacket = 43;
        unlock.live.validFrames = 100;
        bus.publish(unlock);
        // Одного UNLOCK недостаточно: требуется подтверждённый контекст загрузки и исполнения.
        if (wait([&] { return modern.executionPermissionObserved() || old.executionPermissionObserved(); }, 100))
            return 10;
        unlock.readbackVerified = true;
        unlock.live.hasStatus = true;
        unlock.live.status.flags = bcvm::status_flag::FILE_STORED | bcvm::status_flag::CYCLOGRAM_ACTUAL;
        bus.publish(unlock);
        if (!wait([&] {
                return modern.executionPermissionObserved() && old.executionPermissionObserved();
            }))
            return 6;
        // Искусственные отметки и маски UART проверяют независимые физические опоры времени.
        modern.m_boardArmed = true;
        modern.m_hasBoardFrame = true;
        modern.m_boardFrame.channels[7] = {true, false, 500000, 0};
        modern.m_boardFrame.channels[0] = {true, false, 480000, 0};
        modern.m_boardFrame.channels[1] = {true, false, 481000, 0};
        old.m_awaitReset = true;
        old.processLegacyBytes(QByteArray(1, char(0)));
        old.processLegacyBytes(QByteArray(78, char(0)));
        old.processLegacyBytes(QByteArray(1, char(1)));
        old.processLegacyBytes(QByteArray(1, char(3)));
        old.processLegacyBytes(QByteArray(18, char(0)));
        old.processLegacyBytes(QByteArray(1, char(0x80)));
        const auto a = modern.measureAction(modern.plan().actions[0]),
                   b = old.measureAction(old.plan().actions[0]);
        if (a.state != ActionMeasurement::State::Measured ||
            b.state != ActionMeasurement::State::Measured ||
            a.channels[0].actualMs != b.channels[0].actualMs) {
            fprintf(stderr, "new %d %s; old %d %s\n", int(a.state), a.reason.toUtf8().constData(),
                    int(b.state), b.reason.toUtf8().constData());
            return 7;
        }
        const auto modernCollection = modern.collectionId(), oldCollection = old.collectionId();
        bus.publish(prepared);
        if (!wait([&] {
                return modern.collectionId() != modernCollection &&
                       old.collectionId() != oldCollection;
            }))
            return 8;
        if (modern.measurementEligible() || old.measurementEligible())
            return 9;
        QJsonObject result{{"real_kasu_debug_publisher", true},
                           {"real_stand_consumers", 2},
                           {"synthetic_board_inputs", true},
                           {"same_upload_and_digest", true},
                           {"confirmation_and_permission_received", true},
                           {"independent_counter_measurements_ms", *a.channels[0].actualMs},
                           {"new_upload_invalidates_both_collections", true}};
        const auto bytes = QJsonDocument(result).toJson();
        fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
        QFile file("kasu-contract-result.json");
        if (file.open(QIODevice::WriteOnly))
            file.write(bytes);
        return 0;
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    return IntegrationTest::run();
}
