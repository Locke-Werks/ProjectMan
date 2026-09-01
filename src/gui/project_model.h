#pragma once

#include "model.h"

#include <QAbstractTableModel>
#include <QSortFilterProxyModel>

namespace pm::gui {

class ProjectModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        ColName = 0,
        ColBranch,
        ColChanges,
        ColSync,
        ColOpen,
        ColLast,
        ColumnCount,
    };

    enum Role {
        PathRole = Qt::UserRole + 1,
        SortRole,
        GitStateRole,
        ProjectRole,
        DirtyRole,
        OpenRole,
    };

    explicit ProjectModel(QObject* parent = nullptr);

    void setProjects(ProjectList projects);
    void applyStatus(int row, const Project& p);

    const ProjectList& projects() const { return projects_; }
    const Project*     at(int row) const;

    int      rowCount(const QModelIndex& parent = {}) const override;
    int      columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;

private:
    ProjectList projects_;
};

class ProjectFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT

public:
    explicit ProjectFilterProxy(QObject* parent = nullptr);

    void setSearch(const QString& text);
    void setDirtyOnly(bool on);
    void setReposOnly(bool on);

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override;

private:
    QString search_;
    bool    dirtyOnly_ = false;
    bool    reposOnly_ = false;
};

} // namespace pm::gui
