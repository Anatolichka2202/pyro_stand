"""Создаёт отдельную диагностическую копию летнего эталона без сетевых команд."""
import io
import pathlib
import subprocess
import sys
import zipfile

REVISION = "bf13333dec4897accfb6012c958818ed17b6d6cc"


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError(f"Неоднозначный участок эталона: {old[:70]!r}")
    return text.replace(old, new, 1)


def replace_function(text, signature, body):
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[:opening] + "{\n" + body + "\n}" + text[end:]


def prepare(repo, destination):
    # Извлечение фиксированной ревизии: рабочие файлы продукта не меняются.
    archive = subprocess.check_output(["git", "-C", str(repo), "archive", "--format=zip", REVISION])
    destination.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(archive)) as source:
        source.extractall(destination)
    header = destination / "stand.h"
    declaration = header.read_text(encoding="utf-8")
    declaration = replace_once(declaration, "    void sendToBoard();", "    void setCapturePath(const QString &path) { m_capturePath = path; }\n    void sendToBoard();")
    declaration = replace_once(declaration, "    QString m_portName;", "    QString m_portName;\n    QString m_capturePath;")
    header.write_text(declaration, encoding="utf-8")
    path = destination / "stand.cpp"
    text = path.read_text(encoding="utf-8")
    # В этой отдельной сборке сетевые операции отсутствуют даже при вызове API.
    text = replace_function(text, "bool Stand::pingBcvm() const", "    return false;")
    text = replace_function(text, "bool Stand::sendCyclogramTftp(", "    return false;")
    text = replace_function(text, "void Stand::sendToBoard()", '    emit logMessage("Только приём: передача БЦВМ отключена", "system");')
    text = replace_function(text, "void Stand::completeFlight()", "    // В диагностике нет оценки допуска и подстановки планового КП.")
    text = replace_function(text, "void Stand::stop()", '''    m_running = false;
    if (m_worker.joinable()) m_worker.join();
    emit logMessage("Приём остановлен локально; команды БЦВМ не отправлялись", "system");''')
    text = replace_once(text, "#include <QtEndian>", "#include <QtEndian>\n#include <QElapsedTimer>")
    text = replace_once(text, "    m_timeToStartMs = timeToStartMs;", '''    if (m_worker.joinable()) { m_running = false; m_worker.join(); }
    m_timeToStartMs = timeToStartMs;''')
    text = replace_once(text, "    int64_t absoluteIndex = 0;", '''    QFile capture(m_capturePath);
    if (!capture.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        m_port->close();
        emit portError("Не удалось создать новый файл сырых байтов: " + capture.errorString());
        return;
    }
    QElapsedTimer elapsed;
    elapsed.start();
    int64_t absoluteIndex = 0;''')
    text = replace_once(text, "    while (m_running) {", "    while (m_running && elapsed.elapsed() < 180000 && absoluteIndex < 1000000) {")
    text = replace_once(text, "        spuriousCount = 0;\n\n        for", '''        spuriousCount = 0;
        if (capture.write(buf, nRead) != nRead || !capture.flush()) {
            emit portError("Ошибка записи сырых байтов: " + capture.errorString());
            m_running = false;
            break;
        }

        for''')
    # При локальной остановке дочитываем уже сохранённый блок целиком.
    text = replace_once(text, "bi < nRead && m_running", "bi < nRead")
    # Не сохраняем второй неограниченный архив масок в памяти.
    text = replace_once(text, "            m_masks.push_back({absoluteIndex, mask});", '''            // Исходные байты уже сохранены в capture.
            if ((absoluteIndex + 1) % 1000 == 0)
                emit logMessage(QString("COM: %1 байт; маска 0x%2; оценка отключена")
                    .arg(absoluteIndex + 1).arg(mask, 2, 16, QChar('0')), "system");''')
    # Удаляем только блоки планового времени, сохраняя летний декодер масок.
    def erase_braced(signature):
        nonlocal text
        start = text.index(signature)
        opening = text.index("{", start)
        depth, end = 1, opening + 1
        while depth:
            depth += (text[end] == "{") - (text[end] == "}")
            end += 1
        text = text[:start] + text[end:]

    erase_braced("if (absoluteIndex % 1000 == 0)")
    erase_braced("if (!started && absoluteIndex >= timeToStartMs)")
    kp_start = text.index("                const int64_t t0Delay =")
    kp_end = text.index('                if (m_logger) m_logger->log("EVENT", kpMsg', kp_start)
    text = text[:kp_start] + '''                QString kpMsg = QString("[%1] Первый высокий уровень входа 8; физическое назначение КП требует проверки").arg(absoluteIndex);
''' + text[kp_end:]
    text = replace_once(text, "                updatePhase(Phase::Running);", "                // Наличие бита 8 не разрешает оценку допуска в диагностике.")
    text = replace_once(text, "                        if (absoluteIndex < expectedIdx) continue;", "                        // Регистрация групп не зависит от планового времени.")
    erase_braced("else if (absoluteIndex >= expectedIdx + FAIL_MARGIN_MS)")
    erase_braced('else if (ev.status == "fail" && ev.firedTick == -1)')
    finish_start = text.index("            // Выходим когда все события прошли:")
    finish_end = text.index("        } // for bi", finish_start)
    text = text[:finish_start] + text[finish_end:]
    text = replace_once(text, "                        const int64_t expectedIdx = timeToStartMs + ev.time_ms;\n", "")
    text = replace_once(text, "    if (m_syncFound && !m_analysisDone) {", '''    capture.close();
    m_running = false;
    emit logMessage(QString("Приём завершён: %1 байт за %2 мс ПК; шкала 1 байт/мс не проверена, оценка отключена")
        .arg(absoluteIndex).arg(elapsed.elapsed()), "system");
    return;
    if (m_syncFound && !m_analysisDone) {''')
    erase_braced("if (m_syncFound && !m_analysisDone)")
    erase_braced("else if (!m_syncFound && started)")
    # Последний else бывшей ветки завершения больше не имеет условия.
    tail_start = text.index('    return;\n', text.index('    capture.close();'))
    tail_end = text.index('// ─── completeFlight', tail_start)
    text = text[:tail_start] + '}\n\n' + text[tail_end:]
    path.write_text(text, encoding="utf-8")
    window = destination / "mainwindow.cpp"
    ui = window.read_text(encoding="utf-8")
    ui = replace_once(ui, '    m_transferCombo->addItem("TFTP",', '    m_transferCombo->hide();\n    m_transferCombo->addItem("TFTP",')
    ui = replace_once(ui, '        QMessageBox::critical(this, "Ошибка", msg);', '        addLog("Ошибка приёма: " + msg, "system");')
    start = ui.index("    connect(m_stand.get(), &Stand::analysisDone,")
    end = ui.index("\n    });", start) + len("\n    });")
    ui = ui[:start] + '''    connect(m_stand.get(), &Stand::analysisDone, this, [this](const QVector<EventRow> &events) {
        // Загрузка плана не является измерением или оценкой допуска.
        m_displayEvents.clear();
        for (const auto &event : events) if (event.hasChannels) m_displayEvents.append(event);
        updateTable(m_displayEvents);
        resetChannelDots();
        m_summaryStrip->hide();
        m_timeline->setEvents(events);
    });''' + ui[end:]
    ui = replace_once(ui, '    QWidget *timeBox = new QWidget(ctrlCard);', '    QWidget *timeBox = new QWidget(ctrlCard);\n    timeBox->hide();')
    ui = replace_once(ui, '    m_stopBtn->setObjectName("stopBtn");', '    m_stopBtn->setObjectName("stopBtn");\n    m_stopBtn->setMinimumWidth(210);')
    ui = ui.replace('"COM7"', '"COM"')
    ui = replace_once(ui, '    connRow->addWidget(m_bcvmChip);', '    connRow->addWidget(m_bcvmChip);\n    m_bcvmChip->hide();')
    window.write_text(ui, encoding="utf-8")


if __name__ == "__main__":
    prepare(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]))
