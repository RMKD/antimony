#include <Python.h>

#include <QMessageBox>
#include <QKeySequence>
#include <QMouseEvent>
#include <QDebug>
#include <QDirIterator>
#include <QRegExp>

#include "app/app.h"

#include "graph/script_node.h"
#include "graph/graph.h"
#include "graph/datum.h"

#include "ui_main_window.h"
#include "ui/main_window.h"
#include "ui/canvas/canvas.h"
#include "ui/canvas/inspector/inspector.h"
#include "ui/viewport/viewport.h"
#include "ui/script/script_pane.h"

#include "control/proxy.h"

// Initialize global window count (used to detect when last window closes)
int MainWindow::window_count = 0;

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    setAttribute(Qt::WA_DeleteOnClose);

    connectActions(App::instance());
    ui->menuEdit->addAction(App::instance()->undoAction());
    ui->menuEdit->addAction(App::instance()->redoAction());
    setShortcuts();

    populateMenu(ui->menuAdd);

    window_count++;
}

MainWindow::~MainWindow()
{
    window_count--;
    delete ui;
}

void MainWindow::setCentralWidget(QWidget* w)
{
    QMainWindow::setCentralWidget(w);

    if (auto c = dynamic_cast<Canvas*>(centralWidget()))
    {
        c->customizeUI(ui);
        window_type = "Graph";
    }
    else if (auto e = dynamic_cast<ScriptPane*>(centralWidget()))
    {
        e->customizeUI(ui);
        window_type = "Script";
    }
    else
    {
        for (auto v : findChildren<Viewport*>())
            v->customizeUI(ui);
        window_type = "View";
    }
    setWindowTitle(windowTitle().arg(window_type));
}

void MainWindow::connectActions(App* app)
{
    // File menu
    connect(ui->actionSave, &QAction::triggered,
            app, &App::onSave);
    connect(ui->actionSaveAs, &QAction::triggered,
            app, &App::onSaveAs);
    connect(ui->actionNew, &QAction::triggered,
            app, &App::onNew);
    connect(ui->actionOpen, &QAction::triggered,
            app, &App::onOpen);
    connect(ui->actionQuit, &QAction::triggered,
            app, &App::onQuit);

    connect(ui->actionClose, &QAction::triggered,
            this, &MainWindow::tryClose);

    // View window
    connect(ui->actionNewCanvas, &QAction::triggered,
            app, &App::newCanvasWindow);
    connect(ui->actionNewViewport, &QAction::triggered,
            app, &App::newViewportWindow);
    connect(ui->actionNewQuad, &QAction::triggered,
            app, &App::newQuadWindow);

    // Help menu
    connect(ui->actionAbout, &QAction::triggered,
            app, &App::onAbout);
    connect(ui->actionCheckUpdate, &QAction::triggered,
            app, &App::startUpdateCheck);

    // Window title
    setWindowTitle(app->getWindowTitle());
    connect(app, &App::windowTitleChanged, this,
            [=](QString title){
                this->setWindowTitle(title.arg(window_type));});
}

bool MainWindow::askClose()
{
    if(window_count <= 1 && !App::instance()->isUndoStackClean())
    {
        auto res = QMessageBox::question(
                this, "APP_NAME", "There are unsaved changes!\n"
                                  "Do you still want to close this window?\n",
                QMessageBox::No | QMessageBox::Yes, QMessageBox::Yes);

        return (res == QMessageBox::Yes);
    }
    return true;
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (!askClose())
    {
        event->ignore();
    }
}

void MainWindow::tryClose()
{
    if (askClose())
    {
        deleteLater();
    }
}

void MainWindow::setShortcuts()
{
    ui->actionNew->setShortcuts(QKeySequence::New);
    ui->actionOpen->setShortcuts(QKeySequence::Open);
    ui->actionSave->setShortcuts(QKeySequence::Save);
    ui->actionSaveAs->setShortcuts(QKeySequence::SaveAs);
    ui->actionClose->setShortcuts(QKeySequence::Close);
    ui->actionQuit->setShortcuts(QKeySequence::Quit);
    ui->actionCut->setShortcuts(QKeySequence::Cut);
    ui->actionCopy->setShortcuts(QKeySequence::Copy);
    ui->actionPaste->setShortcuts(QKeySequence::Paste);
}

bool MainWindow::isShaded() const
{
    return ui->actionShaded->isChecked();
}

////////////////////////////////////////////////////////////////////////////////

void MainWindow::createNew(bool recenter, NodeConstructorFunction f,
                           Viewport* v)
{
    v = v ? v : findChild<Viewport*>();
    auto c = findChild<Canvas*>();

    Q_ASSERT((v != NULL) ^ (c != NULL));

    QGraphicsView* view = (v != NULL) ?
        static_cast<QGraphicsView*>(v) :
        static_cast<QGraphicsView*>(c);

    if (recenter)
        QCursor::setPos(view->rect().center());

    if (v)
        v->makeNodeAtCursor(f);
    else if (c)
        c->makeNodeAtCursor(f);
}

void MainWindow::addNodeToMenu(QStringList category, QString name, QMenu* menu,
                               bool recenter, NodeConstructorFunction f,
                               Viewport* v)
{
    while (!category.isEmpty())
    {
        bool found = false;
        for (auto m : menu->findChildren<QMenu*>(
                    QString(), Qt::FindDirectChildrenOnly))
        {
            if (m->title() == category.first())
            {
                menu = m;
                found = true;
                break;
            }
        }

        if (!found)
            menu = menu->addMenu(category.first());

        category.removeFirst();
    }
    connect(menu->addAction(name), &QAction::triggered,
            [=]{ this->createNew(recenter, f, v); });
}

void MainWindow::populateNodeMenu(QMenu* menu, bool recenter, Viewport* v)
{
    QDirIterator bitr(App::instance()->bundledNodePath(),
                     QDirIterator::Subdirectories);
    QDirIterator uitr(App::instance()->userNodePath(),
                     QDirIterator::Subdirectories);
    QList<QRegExp> title_regexs= {QRegExp(".*title\\('+([^']+)'+\\).*"),
                                  QRegExp(".*title\\(\"+([^\"]+)\"+\\).*")};

    // Extract all of valid filenames into a QStringList.
    QStringList node_filenames;
    for (auto itr : {&bitr, &uitr})
    {
        while (itr->hasNext())
        {
            auto n = itr->next();
            if (n.endsWith(".node"))
                node_filenames.append(n);
        }
    }

    // Sort the list, then populate menus.
    QMap<QString, QPair<QStringList, NodeConstructorFunction>> nodes;
    QStringList node_titles;
    for (auto n : node_filenames)
    {
        QFile file(n);
        if (!file.open(QIODevice::ReadOnly))
            continue;

        QTextStream in(&file);
        QString txt = in.readAll();

        // Find the menu structure for this node
        auto split = n.split('/');
        while (split.first() != "nodes")
            split.removeFirst();
        split.removeFirst();

        // Attempt to extract the title with a regex;
        // falling back to the node's filename otherwise.
        QString title = split.last().replace(".node","");
        split.removeLast();
        for (auto& regex : title_regexs)
            if (regex.exactMatch(txt))
                title = regex.capturedTexts()[1];

        QString name = "n*";
        if (title.size() > 0 && title.at(0).isLetter())
            name = title.at(0).toLower() + QString("*");
        NodeConstructorFunction constructor =
            [=](Graph *r){ return new ScriptNode(name.toStdString(),
                                                 txt.toStdString(), r); };
        nodes[title] = QPair<QStringList, NodeConstructorFunction>(
                split, constructor);
        node_titles.append(title);
    }

    // Put all of the nodes into the Add menu, deferring Export nodes
    // until the end (after a separator).
    node_titles.sort();
    QStringList deferred;
    for (auto title : node_titles)
        if (nodes[title].first.contains("Export"))
            deferred << title;
        else
            addNodeToMenu(nodes[title].first, title, menu,
                          recenter, nodes[title].second, v);

    menu->addSeparator();
    for (auto title : deferred)
        addNodeToMenu(nodes[title].first, title, menu,
                      recenter, nodes[title].second, v);
}

void MainWindow::populateMenu(QMenu* menu, bool recenter, Viewport* v)
{
    // Hard-code important menu names to set their order.
    for (auto c : {"2D", "3D", "2D → 3D", "3D → 2D", "CSG"})
        menu->addMenu(c);
    menu->addSeparator();

    populateNodeMenu(menu, recenter, v);

    menu->addSeparator();

    addNodeToMenu(QStringList(), "Script", menu, recenter,
                [](Graph *r){ return new ScriptNode("s*",
                    "import fab\n\n"
                    "title('script')\n"
                    "input('r', float, 1)\n"
                    "output('c', fab.shapes.circle(0, 0, r))",
                    r); }, v);
}
