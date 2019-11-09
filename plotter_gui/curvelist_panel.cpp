#include "curvelist_panel.h"
#include "ui_curvelist_panel.h"
#include "PlotJuggler/alphanum.hpp"
#include <QDebug>
#include <QLayoutItem>
#include <QMenu>
#include <QSettings>
#include <QDrag>
#include <QMimeData>
#include <QHeaderView>
#include <QFontDatabase>
#include <QMessageBox>
#include <QApplication>
#include <QPainter>
#include <QCompleter>
#include <QStandardItem>
#include <QWheelEvent>
#include <QItemSelectionModel>
#include <QScrollBar>



//-------------------------------------------------

CurveListPanel::CurveListPanel(const CustomPlotMap &mapped_math_plots,
                               QWidget *parent) :
    QWidget(parent),
    ui(new Ui::CurveListPanel),
    _model(new QStandardItemModel(0, 2, this)),
    _tree_model(new TreeModel(_model)),
    _custom_plots(mapped_math_plots),
    _point_size(9)
{
    ui->setupUi(this);

    _table_view = new CurveTableView(_model, this);

    ui->verticalLayout->insertWidget(3, _table_view->view(), 1 );

    ui->widgetOptions->setVisible(false);

    ui->radioRegExp->setAutoExclusive(true);
    ui->radioContains->setAutoExclusive(true);

    QSettings settings;

    QString active_filter = settings.value("FilterableListWidget.searchFilter").toString();
    if( active_filter == "radioRegExp"){

        ui->radioRegExp->setChecked(true);
    }
    else if( active_filter == "radioContains"){

        ui->radioContains->setChecked(true);
    }

    _point_size = settings.value("FilterableListWidget/table_point_size", 9).toInt();

    ui->splitter->setStretchFactor(0,5);
    ui->splitter->setStretchFactor(1,1);

    ui->treeView->setModel(_tree_model);

    connect(  ui->tableViewCustom->selectionModel(), &QItemSelectionModel::selectionChanged,
              this, &CurveListPanel::onCustomSelectionChanged );
}

CurveListPanel::~CurveListPanel()
{
    delete ui;
}

int CurveListPanel::rowCount() const
{
    return _model->rowCount();
}

void CurveListPanel::clear()
{
    _model->setRowCount(0);
    _tree_model->clear();
    ui->labelNumberDisplayed->setText( "0 of 0");
}

void CurveListPanel::addItem(const QString &item_name)
{
    if( _model->findItems(item_name).size() > 0)
    {
        return;
    }

    auto item = new SortedTableItem(item_name);
    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    font.setPointSize(_point_size);
    item->setFont(font);
    const int row = rowCount();
    _model->setRowCount(row+1);
    _model->setItem(row, 0, item);

    auto val_cell = new QStandardItem("-");
    val_cell->setTextAlignment(Qt::AlignRight);
    val_cell->setFlags( Qt::NoItemFlags | Qt::ItemIsEnabled );
    font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSize( _point_size );
    val_cell->setFont( font );
    val_cell->setFlags(Qt::NoItemFlags);

    _model->setItem(row, 1, val_cell );
    _tree_model->addToTree(item_name, row);
}

void CurveListPanel::refreshColumns()
{
    //TODO ui->tableView->sortByColumn(0,Qt::AscendingOrder);
    ui->treeView->sortByColumn(0,Qt::AscendingOrder);
    ui->tableViewCustom->sortByColumn(0,Qt::AscendingOrder);

    //TODO  ui->tableView->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    ui->treeView->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    //ui->tableViewCustom->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);

    updateFilter();
}


int CurveListPanel::findRowByName(const std::string &text) const
{
    auto item_list = _model->findItems( QString::fromStdString( text ), Qt::MatchExactly);
    if( item_list.isEmpty())
    {
        return -1;
    }
    if( item_list.count()>1)
    {
        qDebug() << "FilterableListWidget constins multiple rows with the same name";
        return -1;
    }
    return item_list.front()->row();
}


void CurveListPanel::updateFilter()
{
    on_lineEdit_textChanged( ui->lineEdit->text() );
}

void CurveListPanel::keyPressEvent(QKeyEvent *event)
{
    if( event->key() == Qt::Key_Delete){
        removeSelectedCurves();
    }
}

QTableView *CurveListPanel::getTableView() const
{
    return dynamic_cast<QTableView*>(_table_view);
}

QTableView *CurveListPanel::getCustomView() const
{
    return ui->tableViewCustom;
}

void CurveListPanel::on_radioContains_toggled(bool checked)
{
    if(checked) {
        updateFilter();
        ui->lineEdit->setCompleter( nullptr );
        QSettings settings;
        settings.setValue("FilterableListWidget.searchFilter", "radioContains");
    }
}

void CurveListPanel::on_radioRegExp_toggled(bool checked)
{
    if(checked) {
        updateFilter();
        ui->lineEdit->setCompleter( nullptr );
        QSettings settings;
        settings.setValue("FilterableListWidget.searchFilter", "radioRegExp");
    }
}

void CurveListPanel::on_checkBoxCaseSensitive_toggled(bool )
{
    updateFilter();
}

void CurveListPanel::on_lineEdit_textChanged(const QString &search_string)
{
    int item_count = rowCount();
    bool updated = false;

    if( ui->radioRegExp->isChecked())
    {
        updated = _table_view->applyVisibilityFilter( CurvesView::REGEX, search_string );
    }
    else if( ui->radioContains->isChecked())
    {
        updated = _table_view->applyVisibilityFilter( CurvesView::CONTAINS, search_string );
    }

// TODO   ui->labelNumberDisplayed->setText( QString::number( visible_count ) + QString(" of ") +
//                                       QString::number( item_count ) );

    if(updated){
        emit hiddenItemsChanged();
    }
}

void CurveListPanel::on_pushButtonSettings_toggled(bool checked)
{
    ui->widgetOptions->setVisible(checked);
}

void CurveListPanel::on_checkBoxHideSecondColumn_toggled(bool checked)
{
    //TODO
//    for(auto table_view: {_table_view->view(), ui->tableViewCustom})
//    {
//        if(checked){
//            table_view->hideColumn(1);
//        }
//        else{
//            table_view->showColumn(1);
//        }
//        table_view->horizontalHeader()->setStretchLastSection( !checked );
//    }

//    emit hiddenItemsChanged();
}

void CurveListPanel::removeSelectedCurves()
{
    QMessageBox::StandardButton reply;
    reply = QMessageBox::question(nullptr, tr("Warning"),
                                  tr("Do you really want to remove these data?\n"),
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No );

    if (reply == QMessageBox::Yes)
    {
        //TODO emit deleteCurves( getNonHiddenSelectedRows() );
    }

    _tree_model->clear();

    for (int row = 0; row < rowCount(); row++)
    {
        auto item = _model->item(row);
        _tree_model->addToTree(item->text(), row);
    }

    updateFilter();
}

void CurveListPanel::removeRow(int row)
{
    _model->removeRow(row);
}

void CurveListPanel::on_buttonAddCustom_clicked()
{
    //TODO
//    auto curve_names = _table_view->getNonHiddenSelectedRows();
//    if( curve_names.empty() )
//    {
//        emit createMathPlot("");
//    }
//    else
//    {
//        createMathPlot( curve_names.front() );
//    }
//    on_lineEdit_textChanged( ui->lineEdit->text() );
}

//void FilterableListWidget::on_buttonRefreshAll_clicked()
//{
//    for(auto& it: _mapped_math_plots)
//    {
//        emit refreshMathPlot( it.second->name() );
//    }
//}

void CurveListPanel::onCustomSelectionChanged(const QItemSelection&, const QItemSelection &)
{
    int selected_items = 0;

    for (const auto &index : ui->tableViewCustom->selectionModel()->selectedRows(0))
    {
        if (! ui->tableViewCustom->isRowHidden(index.row()) )
        {
            selected_items++;
        }
    }

    bool enabled = selected_items == 1;
    ui->buttonEditCustom->setEnabled( enabled );
    ui->buttonEditCustom->setToolTip( enabled ? "Edit the selected custom timeserie" :
                                                "Select a single custom Timeserie to Edit it");
}

void CurveListPanel::on_buttonEditCustom_clicked()
{
    int selected_count = 0;
    QModelIndex selected_index;
    auto table_view = ui->tableViewCustom;

    for (QModelIndex index : table_view->selectionModel()->selectedRows(0))
    {
        if (!table_view->isRowHidden( index.row()) )
        {
            selected_count++;
            selected_index = index;
        }
    }
    if( selected_count != 1)
    {
        return;
    }

    auto item = _model->item( selected_index.row(), 0 );
    editMathPlot( item->text().toStdString() );
}

void CurveListPanel::clearSelections()
{
    ui->tableViewCustom->clearSelection();
    // TODO ui->tableView->clearSelection();
}

void CurveListPanel::on_stylesheetChanged(QString style_dir)
{
    ui->pushButtonSettings->setIcon(QIcon(tr(":/%1/settings_cog.png").arg(style_dir)));
}
