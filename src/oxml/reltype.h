#pragma once
// ============================================================
//  OOXML 命名空间 URI 与 OPC 关系类型常量
// ============================================================

#include <string>

namespace ppt {
namespace xmlns {

const char* const P = "http://schemas.openxmlformats.org/presentationml/2006/main";
const char* const A = "http://schemas.openxmlformats.org/drawingml/2006/main";
const char* const R = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
const char* const MC = "http://schemas.openxmlformats.org/markup-compatibility/2006";
const char* const P14 = "http://schemas.microsoft.com/office/powerpoint/2010/main";
const char* const P15 = "http://schemas.microsoft.com/office/powerpoint/2012/main";

// graphicData uri
const char* const GD_TABLE = "http://schemas.openxmlformats.org/officedocument/2006/tables";
const char* const GD_CHART = "http://schemas.openxmlformats.org/drawingml/2006/chart";
const char* const GD_OLE = "http://schemas.openxmlformats.org/presentationml/2006/ole";

} // namespace xmlns

namespace rel {

const char* const BASE = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";

inline std::string t(const std::string& tail) { return std::string(BASE) + tail; }

// 常用关系类型（比较时用 ends_with(uri, tail)）
const char* const OFFICE_DOCUMENT = "officeDocument";
const char* const SLIDE = "slide";
const char* const SLIDE_LAYOUT = "slideLayout";
const char* const SLIDE_MASTER = "slideMaster";
const char* const NOTES_SLIDE = "notesSlide";
const char* const NOTES_MASTER = "notesMaster";
const char* const HANDOUT_MASTER = "handoutMaster";
const char* const THEME = "theme";
const char* const IMAGE = "image";
const char* const MEDIA = "media";
const char* const CHART = "chart";
const char* const HYPERLINK = "hyperlink";
const char* const TABLE = "table";
const char* const OLE_OBJECT = "oleObject";
const char* const PACKAGE = "package";
const char* const TABLE_STYLES = "tableStyles";
const char* const PRES_PROPS = "presProps";
const char* const VIEW_PROPS = "viewProps";
const char* const PRST_COLORS = "sharedStrings";

// 关系类型尾部匹配
inline bool is(const std::string& type_uri, const char* tail) {
    std::string s = t(tail);
    return type_uri.size() == s.size() ? type_uri == s
                                       : (type_uri.size() > s.size() &&
                                          type_uri.compare(type_uri.size() - s.size(), s.size(), s) == 0);
}

} // namespace rel

// OPC content-type 常用值
namespace ctype {
const char* const SLIDE = "application/vnd.openxmlformats-officedocument.presentationml.slide+xml";
const char* const SLIDE_LAYOUT = "application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml";
const char* const SLIDE_MASTER = "application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml";
const char* const NOTES_SLIDE = "application/vnd.openxmlformats-officedocument.presentationml.notesSlide+xml";
const char* const NOTES_MASTER = "application/vnd.openxmlformats-officedocument.presentationml.notesMaster+xml";
const char* const HANDOUT_MASTER = "application/vnd.openxmlformats-officedocument.presentationml.handoutMaster+xml";
const char* const PRESENTATION = "application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml";
const char* const THEME = "application/vnd.openxmlformats-officedocument.theme+xml";
const char* const CHART = "application/vnd.openxmlformats-officedocument.drawingml.chart+xml";
const char* const CHART_COLORS = "application/vnd.ms-office.chartcolorstyle+xml";
const char* const CHART_STYLE = "application/vnd.ms-office.chartstyle+xml";
const char* const EMBEDDED_PKG = "application/vnd.openxmlformats-officedocument.embedding";
const char* const PACKAGE_RELS = "application/vnd.openxmlformats-package.relationships+xml";
const char* const XML = "application/xml";
} // namespace ctype

} // namespace ppt
