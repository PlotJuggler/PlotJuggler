#ifndef CURVE_SELECTOR_H
#define CURVE_SELECTOR_H

#include <QWidget>
#include <QAction>
#include <QListWidget>
#include <QMouseEvent>
#include <QStandardItemModel>
#include <QTableView>
#include <QItemSelection>

#include "transforms/custom_function.h"
#include "tree_completer.h"
#include "curvetree_view.h"


namespace Ui {
class CurveListPanel;
}

class CurveListPanel : public QWidget
{
    Q_OBJECT

public:
    explicit CurveListPanel(const CustomPlotMap& mapped_math_plots,
                                  QWidget *parent);

    ~CurveListPanel() override;

    void clear();

    void addCurve(const QString& item_name);

    void addCustom(const QString& item_name);

    void refreshColumns();

    int findRowByName(const std::string &text) const;

    void removeRow(int row);

    void rebuildEntireList(const std::vector<std::string> &names);

    void updateFilter();

    void changeFontSize(int point_size);

    bool is2ndColumnHidden() const;

    virtual void keyPressEvent(QKeyEvent * event) override;

private slots:

    void on_radioContains_toggled(bool checked);

    void on_radioRegExp_toggled(bool checked);

    void on_checkBoxCaseSensitive_toggled(bool checked);

    void on_lineEdit_textChanged(const QString &search_string);

    void on_pushButtonSettings_toggled(bool checked);

    void on_checkBoxHideSecondColumn_toggled(bool checked);

    void removeSelectedCurves();

    void on_buttonAddCustom_clicked();

    void on_buttonEditCustom_clicked();

    void onCustomSelectionChanged(const QItemSelection& selected, const QItemSelection& deselected);

public slots:

    void clearSelections();

    void on_stylesheetChanged(QString style_dir);

private:

    Ui::CurveListPanel *ui;

    void updateTreeModel();
    
    CurveTableView* _table_view;
    CurveTableView* _custom_view;
    CurveTreeView* _tree_view;

    const CustomPlotMap& _custom_plots;

signals:

    void hiddenItemsChanged();

    void createMathPlot(const std::string& linked_plot);
    void editMathPlot(const std::string& plot_name);
    void refreshMathPlot(const std::string& curve_name);

    void deleteCurves(const std::vector<std::string>& curve_names);
};

#endif // CURVE_SELECTOR_H
