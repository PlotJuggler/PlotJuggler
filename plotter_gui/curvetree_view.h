#ifndef CURVETREE_VIEW_H
#define CURVETREE_VIEW_H

#include "curvelist_view.h"
#include <QTreeWidget>
#include <functional>

//    void addToTree(const QString& name, int reference_row) {
//        auto parts = name.split('/', QString::SplitBehavior::SkipEmptyParts);
//        if (parts.size() == 0) {
//            return;
//        }

//        TreeItem* tree_parent = &_root_tree_item;

//        for (int i = 0; i < parts.size(); i++) {
//            bool is_leaf = (i == parts.size() - 1);
//            const auto& part = parts[i];

//            TreeItem* matching_child = tree_parent->findChild(part);
//            if (matching_child) {
//                tree_parent = matching_child;
//            } else {

//                tree_parent = tree_parent->appendChild(part);
//                tree_parent->nameItem()->setSelectable(is_leaf);
//            }
//        }
//    }


class CurveTreeView : public QTreeWidget, public CurvesView
{
   public:
    CurveTreeView(CurveListPanel *parent);

    void addItem(const QString& item_name);

    void refreshColumns() override;

    std::vector<std::string> getNonHiddenSelectedRows() override;

    void refreshFontSize() override;

    bool applyVisibilityFilter(FilterType type, const QString& filter_string) override;

    bool eventFilter(QObject* object, QEvent* event) override
    {
        bool ret = CurvesView::eventFilterBase(object, event);
        if(!ret)
        {
            return QWidget::eventFilter(object,event);
        }
        else{
            return true;
        }
    }

    std::pair<int,int> hiddenItemsCount() override
    {
        return { _hidden_count, _leaf_count };
    }

    virtual void hideValuesColumn(bool hide) override
    {
        if(hide){
            hideColumn(1);
        }
        else  {
            showColumn(1);
        }
    }

   void treeVisitor(std::function<void(QTreeWidgetItem *)> visitor);

   private:
    int _hidden_count = 0;
    int _leaf_count = 0;
};


#endif // CURVETREE_VIEW_H
