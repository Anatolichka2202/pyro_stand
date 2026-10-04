#pragma once

#include "integration.h"

#include <QLabel>
#include <QMainWindow>
#include <QTableWidget>
#include <QTextEdit>

class IntegrationWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit IntegrationWindow(IntegrationSession::Options options, const QString &mappingPath = {},
                               QWidget *parent = nullptr);

private:
    void refresh();
    void appendMessage(const QString &text);

    IntegrationSession m_session;
    QLabel *m_debugStatus = nullptr;
    QLabel *m_boardStatus = nullptr;
    QLabel *m_uploadStatus = nullptr;
    QLabel *m_hashStatus = nullptr;
    QLabel *m_bcvmStatus = nullptr;
    QLabel *m_t0Status = nullptr;
    QLabel *m_kpStatus = nullptr;
    QLabel *m_mappingStatus = nullptr;
    QLabel *m_clockStatus = nullptr;
    QTableWidget *m_planTable = nullptr;
    QTableWidget *m_channelTable = nullptr;
    QTextEdit *m_log = nullptr;
};
