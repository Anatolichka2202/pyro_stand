#include "integration_window.h"
#include "bus/kasu_event_bus.h"
#include "core/kasu_controller.h"
#include "debug/bcvm_debug_module.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDir>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QThread>
#include <QTimeZone>
#include <functional>
#include <cstdio>

using namespace kasu;

static bool waitFor(const std::function<bool()> &ready,int milliseconds) {
    QElapsedTimer timer;timer.start();
    while(timer.elapsed()<milliseconds) {
        QCoreApplication::processEvents();
        if(ready()) return true;
        QThread::msleep(2);
    }
    return false;
}
static void log(const QString &text) {
    fprintf(stdout,"%s %s\n",qPrintable(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)),qPrintable(text));
    fflush(stdout);
}

// Доступ только к существующим тестовым швам. Никакой подстановки измерений.
class OperatorGuiTest {
public:
    static int run(const QString &source,const QString &output,int yps) {
        KasuEventBus bus;QThread coreThread;
        auto *controller=new KasuController(&bus);controller->moveToThread(&coreThread);coreThread.start();
        KasuControllerOptions options;options.bindAddress=QHostAddress("192.168.17.231");
        options.port=101;options.peerAddress=QHostAddress("192.168.17.246");options.peerPort=101;
        options.archivePath=QDir(output).filePath("bcvm-exchange.jsonl");
        QString error;bool started=false;
        QMetaObject::invokeMethod(controller,[&]{started=controller->start(options,&error);},Qt::BlockingQueuedConnection);
        if(!started) {
            log("START FAILED: "+error);
            QMetaObject::invokeMethod(controller,[&]{delete controller;},Qt::BlockingQueuedConnection);
            coreThread.quit();coreThread.wait();return 2;
        }
        BcvmUdpExchange *exchange=nullptr;
        QMetaObject::invokeMethod(controller,[&]{exchange=controller->exchange();},Qt::BlockingQueuedConnection);
        BcvmDebugModule debug(&bus,exchange,options.archivePath);
        const QString socket=QStringLiteral("pyro-authorized-hardware-run");
        if(!debug.listenForStand(socket,&error)) {
            log(error);QMetaObject::invokeMethod(controller,[&]{controller->stop();delete controller;},Qt::BlockingQueuedConnection);
            coreThread.quit();coreThread.wait();return 3;
        }
        int requests=0;
        QObject::connect(&bus,&KasuEventBus::published,&bus,[&](const KasuEvent &event){
            if(event.kind==KasuEventKind::OperationRequested) ++requests;
            if(event.kind!=KasuEventKind::LiveState && event.kind!=KasuEventKind::OperationProgress)
                log(QStringLiteral("KASU %1 %2 %3").arg(toString(event.kind),event.action,event.detail));
        });
        IntegrationSession::Options standOptions;standOptions.debugSocket=socket;
        standOptions.firstStageYps=yps;standOptions.kpYps=yps;
        standOptions.comparisonLegacyPort="COM9";standOptions.comparisonLegacySecondStage=true;
        standOptions.comparisonLegacyHasKp=false;
        IntegrationWindow window(standOptions);window.show();
        QObject::connect(&window.m_session,&IntegrationSession::message,&window,[](const QString &s){log("NEW: "+s);});
        const auto state=[&]{QString s;QMetaObject::invokeMethod(controller,[&]{s=exchange->operationState();},Qt::BlockingQueuedConnection);return s;};
        const auto fresh=[&]{bool value=false;QMetaObject::invokeMethod(controller,[&]{value=exchange->liveSnapshot().fresh;},Qt::BlockingQueuedConnection);return value;};
        const auto submit=[&](KasuCommandKind kind,const QString &value=QString()) {
            log(QStringLiteral("REQUEST %1 %2").arg(toString(kind),value));
            const int previous=requests;
            KasuCommand command;command.kind=kind;command.value=value;
            // Подтверждённая команда MODE WAIT: в READY прошивка запрещает перевод часов.
            if(kind==KasuCommandKind::DebugRawBcvmCommand) command.rawBytes={bcvm::command::MODE,bcvm::board_mode::WAIT,0,0,0,0};
            bus.submit(command);
            return waitFor([&]{return requests>previous;},2000);
        };
        const auto execute=[&]()->int {
            if(!waitFor([&]{return fresh() && window.m_session.debugConnected() && window.m_peer &&
                window.m_session.boardReceiving() && window.m_peer->boardReceiving();},30000)) {
                log("NO FRESH BCVM/TESTER/DEBUG: "+state());return 4;
            }
            if(!submit(KasuCommandKind::LockBcvm)) return 5;
            if(!waitFor([&]{return fresh() && window.m_session.lockState()=="LOCKED" && state().contains("BCVM_LOCK_CONFIRMED");},10000)) return 5;
            if(!submit(KasuCommandKind::DebugRawBcvmCommand,"MODE WAIT before UTC synchronization")) return 20;
            if(!waitFor([&]{
                bool waiting=false;
                QMetaObject::invokeMethod(controller,[&]{waiting=bcvm::modeBits(exchange->liveSnapshot().status.flags)==bcvm::board_mode::WAIT;},Qt::BlockingQueuedConnection);
                return fresh() && waiting && state().startsWith("BCVM_EXCHANGE_READY");
            },10000)) return 20;
            if(!submit(KasuCommandKind::SynchronizeBcvmTime)) return 6;
            if(!waitFor([&]{return state().contains("TIME_SYNC_COMMAND_SENT");},10000)) {log(state());return 6;}
            if(!waitFor([&]{
                bcvm::NormalStatus status;
                QMetaObject::invokeMethod(controller,[&]{status=exchange->liveSnapshot().status;},Qt::BlockingQueuedConnection);
                const QByteArray date(reinterpret_cast<const char*>(status.boardDate.data()),int(status.boardDate.size()));
                const QByteArray seconds(reinterpret_cast<const char*>(status.boardTime.data()),int(status.boardTime.size()));
                bool ok=false;const int tick=seconds.toInt(&ok);
                const QDate day=QDate::fromString(QString::fromLatin1(date),"yyyyMMdd");
                const QDateTime utc(day,QTime(0,0).addSecs(tick),QTimeZone::utc());
                return ok && tick>=0 && tick<86400 && day.isValid() && qAbs(utc.msecsTo(QDateTime::currentDateTimeUtc()))<3000;
            },5000)) {log("UTC sync not observed in BCVM status; no upload/unlock");return 19;}
            log("BCVM UTC agrees with PC within 3 seconds (not a metrology claim)");

            QFile input(source);if(!input.open(QIODevice::ReadOnly)) {log(input.errorString());return 7;}
            if(input.size()<=0 || input.size()>qint64(bcvm::CYCLOGRAM_MAX_BYTES)) {log("Source size outside BCVM limits");return 8;}
            const QByteArray original=input.readAll();CyclogramPlan plan;
            if(!StandProtocol::parseCyclogram(original,&plan,&error)) {log(error);return 8;}
            if(plan.actions.isEmpty()) return 8;
            qint32 first=plan.actions.first().offsetMs,last=first;
            for(const auto &action:plan.actions) {first=qMin(first,action.offsetMs);last=qMax(last,action.offsetMs);}
            if(qint64(last)-first>180000) {log("Source exceeds 3min; no implicit rescaling");return 9;}
            const auto t0=QDateTime::fromSecsSinceEpoch(QDateTime::currentSecsSinceEpoch()+90,QTimeZone::utc());
            QString text=QString::fromUtf8(original);
            text.replace(QRegularExpression("(?m)^START_UTC_DATE[^\\r\\n]*"),"START_UTC_DATE = "+t0.toString("yyyy-MM-dd"));
            text.replace(QRegularExpression("(?m)^START_UTC_TIME[^\\r\\n]*"),"START_UTC_TIME = "+t0.toString("HH:mm:ss"));
            const QString filePath=QDir(output).filePath("cyclogram-test.txt");
            QFile file(filePath);const auto bytes=text.toUtf8();
            CyclogramPlan exported;
            if(!StandProtocol::parseCyclogram(bytes,&exported,&error) || exported.t0Utc!=t0 || exported.actions.size()!=plan.actions.size()) {
                log("Export verification failed: "+error);return 10;
            }
            for(qsizetype i=0;i<plan.actions.size();++i) {
                const auto &a=plan.actions[i];const auto &b=exported.actions[i];
                if(a.rawLine!=b.rawLine || a.key!=b.key || a.offsetMs!=b.offsetMs || a.comment!=b.comment) return 10;
            }
            if(!file.open(QIODevice::WriteOnly|QIODevice::NewOnly) || file.write(bytes)!=bytes.size()) return 10;
            file.close();
            log(QStringLiteral("PLAN duration=%1ms T0=%2 SHA256=%3").arg(qint64(last)-first)
                .arg(t0.toString(Qt::ISODate),QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())));
            if(!submit(KasuCommandKind::UploadCyclogram,filePath)) return 11;
            if(!waitFor([&]{return window.m_session.uploadState()=="CONFIRMED";},20000)) {log(state());return 11;}
            if(!submit(KasuCommandKind::ReadCyclogram,QDir(output).filePath("readback.txt"))) return 12;
            if(!waitFor([&]{bool verified=false;QMetaObject::invokeMethod(controller,[&]{verified=exchange->readbackVerified();},Qt::BlockingQueuedConnection);return verified;},20000)) {log(state());return 12;}
            // Загрузка меняет upload_id и сбрасывает прежнее свидетельство LOCK в Pyro.
            if(!submit(KasuCommandKind::LockBcvm) || !waitFor([&]{return fresh() && window.m_session.lockState()=="LOCKED" && state().contains("BCVM_LOCK_CONFIRMED");},10000)) return 5;
            if(QDateTime::currentDateTimeUtc().msecsTo(t0.addMSecs(first))<25000) {log("Not enough time before first action; no UNLOCK");return 13;}
            if(!window.m_session.resetBoard() || !window.m_peer->resetBoard()) return 14;
            if(!waitFor([&]{return window.m_session.boardArmed() && window.m_peer->boardArmed();},5000)) return 15;
            log(QStringLiteral("PRE-UNLOCK fresh=%1 lock=%2 new_rx=%3 old_rx=%4 earliest_in_ms=%5")
                .arg(fresh()).arg(window.m_session.lockState()).arg(window.m_session.boardReceiving())
                .arg(window.m_peer->boardReceiving()).arg(QDateTime::currentDateTimeUtc().msecsTo(t0.addMSecs(first))));
            if(!fresh() || window.m_session.lockState()!="LOCKED" || !window.m_session.boardReceiving() || !window.m_peer->boardReceiving() ||
                QDateTime::currentDateTimeUtc().msecsTo(t0.addMSecs(first))<15000) return 13;
            if(!submit(KasuCommandKind::UnlockBcvm)) return 16;
            if(!waitFor([&]{return window.m_session.executionPermissionObserved() && window.m_peer->executionPermissionObserved();},10000)) {log(state());return 16;}
            log("EXECUTION PERMISSION CONFIRMED; waiting for physical inputs");
            const auto end=t0.addMSecs(last+1500);
            int lastStep=-1;
            while(QDateTime::currentDateTimeUtc()<end) {
                QCoreApplication::processEvents();QThread::msleep(5);
                int step=-1;QMetaObject::invokeMethod(controller,[&]{step=exchange->liveSnapshot().status.cyclogramStep;},Qt::BlockingQueuedConnection);
                if(step!=lastStep) {log(QStringLiteral("BCVM STEP %1").arg(step));lastStep=step;}
                if(!fresh()) {log("BCVM freshness lost; abort");return 17;}
                if(!window.m_session.debugConnected() || !window.m_peer->debugConnected() ||
                    !window.m_session.boardReceiving() || !window.m_peer->boardReceiving()) {
                    log("Measurement/Debug connection lost; abort");return 21;
                }
            }
            return 0;
        };
        const int result=execute();
        // На каждом выходе возвращаем подтверждённый LOCK, включая ошибку после UNLOCK.
        bool locked=false;
        for(int attempt=0;attempt<4 && !locked;++attempt) {
            submit(KasuCommandKind::LockBcvm);
            locked=waitFor([&]{return fresh() && window.m_session.lockState()=="LOCKED" && state().contains("BCVM_LOCK_CONFIRMED");},10000);
        }
        window.m_session.finishCollection(QStringLiteral("Аппаратный тест на имитаторах завершён"));
        if(window.m_peer) window.m_peer->finishCollection(QStringLiteral("Аппаратный тест на имитаторах завершён"));
        window.refresh();const bool screenshotSaved=window.grab().save(QDir(output).filePath("operator-hardware.png"));
        const bool saved=window.m_session.saveReport(QDir(output).filePath("new-report.json"),&error);
        const bool oldSaved=window.m_peer && window.m_peer->saveReport(QDir(output).filePath("old-report.json"),&error);
        QFile status(QDir(output).filePath("result.json"));
        const auto resultBytes=QJsonDocument(QJsonObject{{"result",result},{"final_lock_confirmed",locked},
            {"old_report_saved",oldSaved},{"screenshot_saved",screenshotSaved},
            {"new_report_saved",saved},{"new_frames",QString::number(window.m_session.acceptedFrames())},
            {"new_crc_errors",QString::number(window.m_session.rejectedFrames())},{"yps",yps},
            {"safe_loads_confirmed_by_operator",true},{"error",error}}).toJson();
        const bool statusSaved=status.open(QIODevice::WriteOnly|QIODevice::NewOnly) && status.write(resultBytes)==resultBytes.size() && status.flush();
        QMetaObject::invokeMethod(controller,[&]{controller->stop();delete controller;},Qt::BlockingQueuedConnection);
        coreThread.quit();coreThread.wait();log(QStringLiteral("RESULT %1 LOCK=%2").arg(result).arg(locked));
        return result ? result : locked && saved && oldSaved && screenshotSaved && statusSaved ? 0 : 18;
    }
};

int main(int argc,char **argv) {
    QApplication app(argc,argv);
#ifdef Q_OS_WIN
    // Offscreen-плагин не находит системные шрифты автоматически.
    QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR")+"/Fonts/segoeui.ttf");
    app.setFont(QFont(QStringLiteral("Segoe UI"),9));
#endif
    QCommandLineParser parser;parser.addHelpOption();
    parser.addOption({"execute-safe-loads",QStringLiteral("Явно разрешённый запуск на безопасных имитаторах")});
    parser.addOption({"source","Исходное ПЗ","path"});parser.addOption({"output","Новый каталог результатов","path"});
    parser.addOption({"yps","ЯПС новой платы, 1..12","number","8"});parser.process(app);
    bool ok=false;const int yps=parser.value("yps").toInt(&ok);
    if(!parser.isSet("execute-safe-loads") || !parser.isSet("source") || !parser.isSet("output") || !ok || yps<1 || yps>12) parser.showHelp(1);
    const auto output=QFileInfo(parser.value("output")).absoluteFilePath();
    if(QFileInfo::exists(output) || !QDir().mkpath(output)) return 1;
    return OperatorGuiTest::run(QFileInfo(parser.value("source")).absoluteFilePath(),output,yps);
}
