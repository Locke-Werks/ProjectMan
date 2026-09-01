#include "project_model.h"

#include "strutil.h"

#include <QDateTime>

namespace pm::gui {
namespace {

QString changesText(const GitStatus& g)
{
    switch (g.state) {
    case GitState::Dirty:    return QString::number(g.dirtyCount());
    case GitState::Clean:    return QStringLiteral("clean");
    case GitState::Bare:     return QStringLiteral("bare");
    case GitState::Error:    return QStringLiteral("error");
    case GitState::Unknown:  return QStringLiteral("...");
    case GitState::NotARepo: return {};
    }
    return {};
}

QString syncText(const GitStatus& g)
{
    if (g.state == GitState::NotARepo || g.state == GitState::Bare)
        return {};
    if (!g.hasUpstream)
        return QStringLiteral("no upstream");

    QString s;
    if (g.ahead)
        s += QStringLiteral("+%1").arg(g.ahead);
    if (g.behind)
        s += (s.isEmpty() ? QString() : QStringLiteral(" ")) + QStringLiteral("-%1").arg(g.behind);
    return s.isEmpty() ? QStringLiteral("synced") : s;
}

} // namespace

ProjectModel::ProjectModel(QObject* parent) : QAbstractTableModel(parent) {}

void ProjectModel::setProjects(ProjectList projects)
{
    beginResetModel();
    projects_ = std::move(projects);
    endResetModel();
}

void ProjectModel::applyStatus(int row, const Project& p)
{
    if (row < 0 || row >= static_cast<int>(projects_.size()))
        return;
    projects_[static_cast<size_t>(row)] = p;
    // One row, not a reset. A reset would drop the selection and scroll
    // position on every one of 113 streaming results.
    emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
}

const Project* ProjectModel::at(int row) const
{
    if (row < 0 || row >= static_cast<int>(projects_.size()))
        return nullptr;
    return &projects_[static_cast<size_t>(row)];
}

int ProjectModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(projects_.size());
}

int ProjectModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ProjectModel::data(const QModelIndex& index, int role) const
{
    const Project* p = at(index.row());
    if (!p)
        return {};

    switch (role) {
    case PathRole:     return QString::fromStdString(p->path.string());
    case GitStateRole: return static_cast<int>(p->git.state);
    case DirtyRole:    return p->git.dirtyCount();
    case OpenRole:     return p->open.total();
    case ProjectRole:  return QVariant::fromValue<const void*>(p);
    default:           break;
    }

    if (role == SortRole) {
        switch (index.column()) {
        case ColName:    return QString::fromStdString(p->displayName()).toLower();
        case ColChanges: return p->git.dirtyCount();
        case ColOpen:    return p->open.total();
        case ColLast:    return QVariant::fromValue<qint64>(p->activityUnix());
        default:         break;
        }
    }

    if (role != Qt::DisplayRole && role != Qt::ToolTipRole)
        return {};

    if (role == Qt::ToolTipRole)
        return QString::fromStdString(p->path.string());

    switch (index.column()) {
    case ColName:    return QString::fromStdString(p->displayName());
    case ColBranch:  return QString::fromStdString(p->git.branch);
    case ColChanges: return changesText(p->git);
    case ColSync:    return syncText(p->git);
    case ColOpen:    return p->open.total() ? QString::number(p->open.total()) : QString();
    case ColLast:    return QString::fromStdString(relativeAge(p->activityUnix()));
    default:         return {};
    }
}

QVariant ProjectModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal || role != Qt::DisplayRole)
        return {};

    switch (section) {
    case ColName:    return QStringLiteral("PROJECT");
    case ColBranch:  return QStringLiteral("BRANCH");
    case ColChanges: return QStringLiteral("CHANGES");
    case ColSync:    return QStringLiteral("SYNC");
    case ColOpen:    return QStringLiteral("OPEN");
    case ColLast:    return QStringLiteral("LAST");
    default:         return {};
    }
}

ProjectFilterProxy::ProjectFilterProxy(QObject* parent) : QSortFilterProxyModel(parent)
{
    setSortRole(ProjectModel::SortRole);
    // Streaming statuses have to re-sort the row they land on, or a sweep
    // sorted by "recent" stays in discovery order until something else nudges
    // it.
    setDynamicSortFilter(true);
}

void ProjectFilterProxy::setSearch(const QString& text)
{
    search_ = text;
    invalidateFilter();
}

void ProjectFilterProxy::setDirtyOnly(bool on)
{
    dirtyOnly_ = on;
    invalidateFilter();
}

void ProjectFilterProxy::setReposOnly(bool on)
{
    reposOnly_ = on;
    invalidateFilter();
}

bool ProjectFilterProxy::filterAcceptsRow(int row, const QModelIndex& parent) const
{
    const QModelIndex idx = sourceModel()->index(row, ProjectModel::ColName, parent);

    if (dirtyOnly_
        && idx.data(ProjectModel::GitStateRole).toInt()
               != static_cast<int>(GitState::Dirty)) {
        return false;
    }

    if (reposOnly_) {
        const int st = idx.data(ProjectModel::GitStateRole).toInt();
        if (st == static_cast<int>(GitState::NotARepo))
            return false;
    }

    if (search_.isEmpty())
        return true;

    return idx.data(Qt::DisplayRole).toString().contains(search_, Qt::CaseInsensitive)
        || idx.data(ProjectModel::PathRole).toString().contains(search_,
                                                                Qt::CaseInsensitive);
}

} // namespace pm::gui
