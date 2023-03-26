#ifndef RUNDIALOG_H
#define RUNDIALOG_H

#include <QDialog>

QT_BEGIN_NAMESPACE
namespace Ui { class RunDialog; }
QT_END_NAMESPACE

class RunDialog : public QDialog
{
    Q_OBJECT

public:
    RunDialog(QWidget *parent = nullptr);
    ~RunDialog();

private slots:
    void on_actionrunAct_triggered();

private:
    Ui::RunDialog *ui;
};
#endif // RUNDIALOG_H
