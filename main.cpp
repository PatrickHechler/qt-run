#include "rundialog.h"

#include <QApplication>

#include <stdio.h>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    RunDialog w;
    w.show();
    return a.exec();
}
