/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#include "mutation_partition.hh"


mutation_partition::~mutation_partition() {
    _rows.clear_and_dispose(std::default_delete<rows_entry>());
    _row_tombstones.clear_and_dispose(std::default_delete<row_tombstones_entry>());
}

void
mutation_partition::apply(schema_ptr schema, const mutation_partition& p) {
    _tombstone.apply(p._tombstone);

    for (auto&& e : p._row_tombstones) {
        apply_row_tombstone(schema, e.prefix(), e.t());
    }

    auto merge_cells = [this, schema] (row& old_row, const row& new_row, auto&& find_column_def) {
        for (auto&& new_column : new_row) {
            auto col = new_column.first;
            auto i = old_row.find(col);
            if (i == old_row.end()) {
                old_row.emplace_hint(i, new_column);
            } else {
                auto& old_column = *i;
                auto& def = find_column_def(col);
                merge_column(def, old_column.second, new_column.second);
            }
        }
    };

    auto find_static_column_def = [schema] (auto col) -> const column_definition& { return schema->static_column_at(col); };
    auto find_regular_column_def = [schema] (auto col) -> const column_definition& { return schema->regular_column_at(col); };

    merge_cells(_static_row, p._static_row, find_static_column_def);

    for (auto&& entry : p._rows) {
        auto& key = entry.key();
        auto i = _rows.find(key, rows_entry::compare(*schema));
        if (i == _rows.end()) {
            auto e = new rows_entry(entry);
            _rows.insert(i, *e);
        } else {
            i->row().t.apply(entry.row().t);
            i->row().created_at = std::max(i->row().created_at, entry.row().created_at);
            merge_cells(i->row().cells, entry.row().cells, find_regular_column_def);
        }
    }
}

tombstone
mutation_partition::range_tombstone_for_row(const schema& schema, const clustering_key& key) const {
    tombstone t = _tombstone;

    if (_row_tombstones.empty()) {
        return t;
    }

    auto c = row_tombstones_entry::key_comparator(
        clustering_key::prefix_view_type::less_compare_with_prefix(schema));

    // _row_tombstones contains only strict prefixes
    for (unsigned prefix_len = 1; prefix_len < schema.clustering_key_size(); ++prefix_len) {
        auto i = _row_tombstones.find(key.prefix_view(schema, prefix_len), c);
        if (i != _row_tombstones.end()) {
            t.apply(i->t());
        }
    }

    return t;
}

tombstone
mutation_partition::tombstone_for_row(const schema& schema, const clustering_key& key) const {
    tombstone t = range_tombstone_for_row(schema, key);

    auto j = _rows.find(key, rows_entry::compare(schema));
    if (j != _rows.end()) {
        t.apply(j->row().t);
    }

    return t;
}

tombstone
mutation_partition::tombstone_for_row(const schema& schema, const rows_entry& e) const {
    tombstone t = range_tombstone_for_row(schema, e.key());
    t.apply(e.row().t);
    return t;
}

void
mutation_partition::apply_row_tombstone(schema_ptr schema, clustering_key_prefix prefix, tombstone t) {
    assert(!prefix.is_full(*schema));
    auto i = _row_tombstones.lower_bound(prefix, row_tombstones_entry::compare(*schema));
    if (i == _row_tombstones.end() || !prefix.equal(*schema, i->prefix())) {
        auto e = new row_tombstones_entry(std::move(prefix), t);
        _row_tombstones.insert(i, *e);
    } else {
        i->apply(t);
    }
}

void
mutation_partition::apply_delete(schema_ptr schema, const exploded_clustering_prefix& prefix, tombstone t) {
    if (!prefix) {
        apply(t);
    } else if (prefix.is_full(*schema)) {
        apply_delete(schema, clustering_key::from_clustering_prefix(*schema, prefix), t);
    } else {
        apply_row_tombstone(schema, clustering_key_prefix::from_clustering_prefix(*schema, prefix), t);
    }
}

void
mutation_partition::apply_delete(schema_ptr schema, clustering_key&& key, tombstone t) {
    auto i = _rows.lower_bound(key, rows_entry::compare(*schema));
    if (i == _rows.end() || !i->key().equal(*schema, key)) {
        auto e = new rows_entry(std::move(key));
        e->row().apply(t);
        _rows.insert(i, *e);
    } else {
        i->row().apply(t);
    }
}

rows_entry*
mutation_partition::find_entry(schema_ptr schema, const clustering_key_prefix& key) {
    auto i = _rows.find(key, rows_entry::key_comparator(clustering_key::less_compare_with_prefix(*schema)));
    if (i == _rows.end()) {
        return nullptr;
    }
    return &*i;
}

row*
mutation_partition::find_row(const clustering_key& key) {
    auto i = _rows.find(key);
    if (i == _rows.end()) {
        return nullptr;
    }
    return &i->row().cells;
}

deletable_row&
mutation_partition::clustered_row(const clustering_key& key) {
    auto i = _rows.find(key);
    if (i == _rows.end()) {
        auto e = new rows_entry(key);
        _rows.insert(i, *e);
        return e->row();
    }
    return i->row();
}


boost::iterator_range<mutation_partition::rows_type::const_iterator>
mutation_partition::range(const schema& schema, const query::range<clustering_key_prefix>& r) const {
    if (r.is_full()) {
        return boost::make_iterator_range(_rows.cbegin(), _rows.cend());
    }
    auto cmp = rows_entry::key_comparator(clustering_key::prefix_equality_less_compare(schema));
    if (r.is_singular()) {
        auto&& prefix = r.start()->value();
        return boost::make_iterator_range(_rows.lower_bound(prefix, cmp), _rows.upper_bound(prefix, cmp));
    }
    auto i1 = r.start() ? (r.start()->is_inclusive()
            ? _rows.lower_bound(r.start()->value(), cmp)
            : _rows.upper_bound(r.start()->value(), cmp)) : _rows.cbegin();
    auto i2 = r.end() ? (r.end()->is_inclusive()
            ? _rows.upper_bound(r.end()->value(), cmp)
            : _rows.lower_bound(r.end()->value(), cmp)) : _rows.cend();
    return boost::make_iterator_range(i1, i2);
}


template <typename ColumnDefResolver>
static void get_row_slice(const row& cells, const std::vector<column_id>& columns, tombstone tomb,
        ColumnDefResolver&& id_to_def, query::result::row_writer& writer) {
    for (auto id : columns) {
        auto i = cells.find(id);
        if (i == cells.end()) {
            writer.add_empty();
        } else {
            auto&& def = id_to_def(id);
            if (def.is_atomic()) {
                auto c = i->second.as_atomic_cell();
                if (!c.is_live(tomb)) {
                    writer.add_empty();
                } else {
                    writer.add(i->second.as_atomic_cell());
                }
            } else {
                auto&& cell = i->second.as_collection_mutation();
                auto&& ctype = static_pointer_cast<const collection_type_impl>(def.type);
                auto m_view = ctype->deserialize_mutation_form(cell);
                m_view.tomb.apply(tomb);
                auto m_ser = ctype->serialize_mutation_form_only_live(m_view);
                if (ctype->is_empty(m_ser)) {
                    writer.add_empty();
                } else {
                    writer.add(m_ser);
                }
            }
        }
    }
}

template <typename ColumnDefResolver>
bool has_any_live_data(const row& cells, tombstone tomb, ColumnDefResolver&& id_to_def) {
    for (auto&& e : cells) {
        auto&& cell_or_collection = e.second;
        const column_definition& def = id_to_def(e.first);
        if (def.is_atomic()) {
            auto&& c = cell_or_collection.as_atomic_cell();
            if (c.is_live(tomb)) {
                return true;
            }
        } else {
            auto&& cell = cell_or_collection.as_collection_mutation();
            auto&& ctype = static_pointer_cast<const collection_type_impl>(def.type);
            if (ctype->is_any_live(cell, tomb)) {
                return true;
            }
        }
    }
    return false;
}

void
mutation_partition::query(const schema& s,
    const query::partition_slice& slice,
    uint32_t limit,
    query::result::partition_writer& pw) const
{
    auto regular_column_resolver = [&s] (column_id id) -> const column_definition& {
        return s.regular_column_at(id);
    };

    // So that we can always add static row before we know how many clustered rows there will be,
    // without exceeding the limit.
    assert(limit > 0);

    if (!slice.static_columns.empty()) {
        auto static_column_resolver = [&s] (column_id id) -> const column_definition& {
            return s.static_column_at(id);
        };
        auto row_builder = pw.add_static_row();
        get_row_slice(static_row(), slice.static_columns, tombstone_for_static_row(),
            static_column_resolver, row_builder);
        row_builder.finish();
    }

    for (auto&& row_range : slice.row_ranges) {
        if (limit == 0) {
            break;
        }

        // FIXME: Optimize for a full-tuple singular range. mutation_partition::range()
        // does two lookups to form a range, even for singular range. We need
        // only one lookup for a full-tuple singular range though.
        for (const rows_entry& e : range(s, row_range)) {
            auto& row = e.row();
            auto&& cells = row.cells;

            auto row_tombstone = tombstone_for_row(s, e);
            auto row_is_live = row.created_at > row_tombstone.timestamp;

            // row_is_live is true for rows created using 'insert' statement
            // which are not deleted yet. Such rows are considered as present
            // even if no regular columns are live. Otherwise, a row is
            // considered present if it has any cell which is live. So if
            // we've got no live cell in the results we still have to check if
            // any of the row's cell is live and we should return the row in
            // such case.
            if (row_is_live || has_any_live_data(cells, row_tombstone, regular_column_resolver)) {
                auto row_builder = pw.add_row(e.key());
                get_row_slice(cells, slice.regular_columns, row_tombstone, regular_column_resolver, row_builder);
                row_builder.finish();
                if (--limit == 0) {
                    break;
                }
            }
        }
    }
}