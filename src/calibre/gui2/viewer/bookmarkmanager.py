#!/usr/bin/env python
# vim:fileencoding=utf-8
from __future__ import (unicode_literals, division, absolute_import,
                        print_function)

__license__ = 'GPL v3'
__copyright__ = '2013, Kovid Goyal <kovid at kovidgoyal.net>'

import cPickle

from PyQt5.Qt import (
    Qt, QDialog, QListWidgetItem, QItemSelectionModel)

from calibre.gui2 import choose_save_file, choose_files
from calibre.gui2.viewer.bookmarkmanager_ui import Ui_BookmarkManager

class BookmarkManager(QDialog, Ui_BookmarkManager):
    def __init__(self, parent, bookmarks):
        QDialog.__init__(self, parent)

        self.setupUi(self)

        self.original_bookmarks = bookmarks
        self.set_bookmarks()

        self.button_revert.clicked.connect(lambda :self.set_bookmarks())
        self.button_delete.clicked.connect(self.delete_bookmark)
        self.button_edit.clicked.connect(self.edit_bookmark)
        self.button_export.clicked.connect(self.export_bookmarks)
        self.button_import.clicked.connect(self.import_bookmarks)
        self.bookmarks_list.setStyleSheet('QListView::item { padding: 0.5ex }')
        self.bookmarks_list.viewport().setAcceptDrops(True)
        self.bookmarks_list.setDropIndicatorShown(True)
        self.bookmarks_list.itemChanged.connect(self.item_changed)
        self.resize(600, 500)
        self.bookmarks_list.setFocus(Qt.OtherFocusReason)

    def set_bookmarks(self, bookmarks=None):
        if bookmarks is None:
            bookmarks = self.original_bookmarks
        self.bookmarks_list.clear()
        for bm in bookmarks:
            i = QListWidgetItem(bm['title'])
            i.setData(Qt.UserRole, self.bm_to_item(bm))
            i.setFlags(i.flags() | Qt.ItemIsEditable)
            self.bookmarks_list.addItem(i)
        if len(bookmarks) > 0:
            self.bookmarks_list.setCurrentItem(self.bookmarks_list.item(0), QItemSelectionModel.ClearAndSelect)

    def item_changed(self, item):
        self.bookmarks_list.blockSignals(True)
        title = unicode(item.data(Qt.DisplayRole))
        if not title:
            title = _('Unknown')
            item.setData(Qt.DisplayRole, title)
        bm = self.item_to_bm(item)
        bm['title'] = title
        item.setData(Qt.UserRole, self.bm_to_item(bm))
        self.bookmarks_list.blockSignals(False)

    def delete_bookmark(self):
        row = self.bookmarks_list.currentRow()
        if row > -1:
            self.bookmarks_list.takeItem(row)

    def edit_bookmark(self):
        item = self.bookmarks_list.currentItem()
        if item is not None:
            self.bookmarks_list.editItem(item)

    def bm_to_item(self, bm):
        return bytearray(cPickle.dumps(bm, -1))

    def item_to_bm(self, item):
        return cPickle.loads(bytes(item.data(Qt.UserRole)))

    def get_bookmarks(self):
        l = self.bookmarks_list
        return [self.item_to_bm(l.item(i)) for i in xrange(l.count())]

    def export_bookmarks(self):
        path = choose_save_file(self, 'export_viewer_bookmarks', _("Export Bookmarks"),
                                filters=[(_('Saved Bookmarks'), ['pickle'])])
        if not path:
            return
        if not path.endswith('.pickle'):
            path += '.pickle'

        with open(path, 'w') as fileobj:
            cPickle.dump(self.get_bookmarks(), fileobj)

    def import_bookmarks(self):
        paths = choose_files(self, 'import_viewer_bookmarks', _('Import Bookmarks'),
                             filters=[(_('Saved Bookmarks'), ['pickle'])], all_files=False, select_only_single_file=True)
        if not paths:
            return
        filename = paths[0]

        imported = None
        with open(filename, 'r') as fileobj:
            imported = cPickle.load(fileobj)

        if imported is not None:
            bad = False
            try:
                for bm in imported:
                    if 'title' not in bm:
                        bad = True
                        break
            except:
                pass

            if not bad:
                bookmarks = self.get_bookmarks()
                for bm in imported:
                    if bm not in bookmarks:
                        bookmarks.append(bm)
                self.set_bookmarks([bm for bm in bookmarks if bm['title'] != 'calibre_current_page_bookmark'])

if __name__ == '__main__':
    from PyQt5.Qt import QApplication
    app = QApplication([])
    d = BookmarkManager(None, [{'title':'Bookmark #%d' % i, 'data':b'xxxxx'} for i in range(1, 5)])
    d.exec_()
    import pprint
    pprint.pprint(d.get_bookmarks())


