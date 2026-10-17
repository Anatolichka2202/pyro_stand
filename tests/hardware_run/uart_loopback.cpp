#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSerialPort>
#include <cstdio>

int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);QCommandLineParser parser;parser.addHelpOption();
    parser.addOption({"isolated-loopback-confirmed",QStringLiteral("Адаптер отключён от плат, только его TX замкнут на RX")});
    parser.addOption({"port",QStringLiteral("Порт адаптера"),"port","COM9"});
    parser.addOption({"output",QStringLiteral("Новый файл результата"),"path"});
    parser.process(app);
    // Тестовые байты запрещены до явного подтверждения физической изоляции.
    if(!parser.isSet("isolated-loopback-confirmed") || !parser.isSet("output")) parser.showHelp(1);
    QFile output(parser.value("output"));
    if(!output.open(QIODevice::WriteOnly|QIODevice::NewOnly)) return 1;
    QSerialPort serial;serial.setPortName(parser.value("port"));
    serial.setBaudRate(115200);serial.setDataBits(QSerialPort::Data8);
    serial.setParity(QSerialPort::NoParity);serial.setStopBits(QSerialPort::OneStop);
    serial.setFlowControl(QSerialPort::NoFlowControl);
    QJsonObject result{{"format","pyro_stand.uart-loopback.v1"},{"port",parser.value("port")},
        {"baud",115200},{"isolated_adapter_confirmed",true},
        {"at_utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    QByteArray sent,received;QString error;
    if(!serial.open(QIODevice::ReadWrite)) error=serial.errorString();
    else if(!serial.clear(QSerialPort::AllDirections)) error=serial.errorString();
    else {
        // Все 256 значений проверяют также нули, старший бит и символы XON/XOFF.
        for(int repeat=0;repeat<4;++repeat) for(int value=0;value<256;++value) sent.append(char(value));
        if(serial.write(sent)!=sent.size()) error=serial.errorString();
        QElapsedTimer deadline;deadline.start();
        while(error.isEmpty() && deadline.elapsed()<2500 && received.size()<sent.size()) {
            serial.waitForReadyRead(50);received+=serial.readAll();
            if(serial.error()!=QSerialPort::NoError && serial.error()!=QSerialPort::TimeoutError) error=serial.errorString();
        }
        // Лишние байты также должны делать проверку неуспешной.
        if(serial.waitForReadyRead(100)) received+=serial.readAll();
    }
    serial.close();
    const bool passed=error.isEmpty() && !sent.isEmpty() && sent==received;
    result.insert("passed",passed);result.insert("error",error);
    result.insert("sent_base64",QString::fromLatin1(sent.toBase64()));
    result.insert("received_base64",QString::fromLatin1(received.toBase64()));
    result.insert("sent_bytes",sent.size());result.insert("received_bytes",received.size());
    const auto bytes=QJsonDocument(result).toJson();
    if(output.write(bytes)!=bytes.size() || !output.flush()) return 3;
    fprintf(stdout,"loopback passed=%d sent=%lld received=%lld\n",int(passed),
        static_cast<long long>(sent.size()),static_cast<long long>(received.size()));
    return passed?0:2;
}
