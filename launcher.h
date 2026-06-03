#ifndef LAUNCHER_H
#define LAUNCHER_H

#include <QWidget>

namespace Ui {
class launcher;
}

class launcher : public QWidget
{
    Q_OBJECT

public:
    explicit launcher(QWidget *parent = nullptr);
    ~launcher();

signals:
    void yps_test();
    void emulator();

private slots:
    void on_pushButton_2_clicked();

private:
    Ui::launcher *ui;
};

#endif // LAUNCHER_H
