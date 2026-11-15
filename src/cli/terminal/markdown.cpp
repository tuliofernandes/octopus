#include "cli/terminal/named_entities.hpp"
#include "cli/terminal/text.hpp"
#include "octopus/cli/terminal_renderer.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <md4c.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace octopus {
namespace {
constexpr std::string_view kReplacementCharacter = "\xEF\xBF\xBD";
constexpr std::size_t kMaximumMarkdownBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumNesting = 64;
constexpr std::size_t kMaximumTableCells = 16384;

void appendUtf8(std::string &output, std::uint32_t value) {
    if (value <= 0x7FU) {
        output.push_back(static_cast<char>(value));
    } else if (value <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | (value >> 6U)));
        output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    } else if (value <= 0xFFFFU) {
        output.push_back(static_cast<char>(0xE0U | (value >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    } else {
        output.push_back(static_cast<char>(0xF0U | (value >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    }
}

std::string decodeEntity(std::string_view entity) {
    if (entity.size() >= 3 && entity.front() == '&' && entity.back() == ';' && entity[1] != '#') {
        const auto name = entity.substr(1, entity.size() - 2);
        const auto &entries = cli_detail::kNamedEntities;
        const auto found = std::lower_bound(
            entries.begin(), entries.end(), name,
            [](const auto &entry, std::string_view key) { return entry.first < key; });
        return found != entries.end() && found->first == name ? std::string(found->second)
                                                              : std::string(entity);
    }
    if (entity.size() < 4 || entity.front() != '&' || entity[1] != '#' || entity.back() != ';') {
        return std::string(entity);
    }

    std::size_t offset = 2;
    int base = 10;
    if (offset < entity.size() && (entity[offset] == 'x' || entity[offset] == 'X')) {
        base = 16;
        ++offset;
    }
    std::uint32_t value = 0;
    bool has_digit = false;
    for (; offset + 1 < entity.size(); ++offset) {
        const unsigned char byte = static_cast<unsigned char>(entity[offset]);
        unsigned digit = 0;
        if (byte >= '0' && byte <= '9') {
            digit = byte - '0';
        } else if (base == 16 && byte >= 'a' && byte <= 'f') {
            digit = byte - 'a' + 10U;
        } else if (base == 16 && byte >= 'A' && byte <= 'F') {
            digit = byte - 'A' + 10U;
        } else {
            return std::string(entity);
        }
        has_digit = true;
        if (value > (0x10FFFFU - digit) / static_cast<unsigned>(base)) {
            return std::string(kReplacementCharacter);
        }
        value = value * static_cast<unsigned>(base) + digit;
    }
    if (!has_digit || value == 0 || value > 0x10FFFFU || (value >= 0xD800U && value <= 0xDFFFU)) {
        return std::string(kReplacementCharacter);
    }
    std::string result;
    appendUtf8(result, value);
    return result;
}

std::string copyAttribute(const MD_ATTRIBUTE &attribute) {
    if (attribute.text == nullptr || attribute.size == 0) {
        return {};
    }
    if (attribute.substr_types == nullptr || attribute.substr_offsets == nullptr) {
        return std::string(attribute.text, attribute.size);
    }

    std::string result;
    std::size_t index = 0;
    while (attribute.substr_offsets[index] < attribute.size) {
        const std::size_t begin = attribute.substr_offsets[index];
        const std::size_t end = attribute.substr_offsets[index + 1];
        if (end < begin || end > attribute.size) {
            return {};
        }
        const std::string_view part(attribute.text + begin, end - begin);
        if (attribute.substr_types[index] == MD_TEXT_ENTITY) {
            result += decodeEntity(part);
        } else if (attribute.substr_types[index] == MD_TEXT_NULLCHAR) {
            result += kReplacementCharacter;
        } else {
            result.append(part);
        }
        ++index;
    }
    return result;
}

bool lineEmpty(const TerminalLine &line) noexcept { return line.runs.empty(); }

class MarkdownBuilder final {
  public:
    explicit MarkdownBuilder(std::optional<std::size_t> columns, bool utf8)
        : columns_(columns), utf8_(utf8) {}

    int enterBlock(MD_BLOCKTYPE type, void *detail) {
        switch (type) {
        case MD_BLOCK_DOC:
            break;
        case MD_BLOCK_QUOTE:
            separateBlock();
            if (++quote_depth_ > kMaximumNesting) {
                return fail();
            }
            break;
        case MD_BLOCK_UL:
            separateListIfNeeded();
            if (lists_.size() >= kMaximumNesting) {
                return fail();
            }
            lists_.push_back({false, 1});
            break;
        case MD_BLOCK_OL: {
            separateListIfNeeded();
            if (lists_.size() >= kMaximumNesting || detail == nullptr) {
                return fail();
            }
            const auto &ordered = *static_cast<MD_BLOCK_OL_DETAIL *>(detail);
            lists_.push_back({true, ordered.start});
            break;
        }
        case MD_BLOCK_LI:
            if (++list_item_depth_ > kMaximumNesting || lists_.empty()) {
                return fail();
            }
            if (!lineEmpty(current_)) {
                finishLine();
            }
            addQuotePrefix();
            if (lists_.size() > 1) {
                appendRun(std::string((lists_.size() - 1) * 2, ' '), {});
            }
            appendListMarker(detail);
            break;
        case MD_BLOCK_H:
            separateBlock();
            if (heading_depth_ >= kMaximumNesting) {
                return fail();
            }
            ++heading_depth_;
            ensureLinePrefix();
            break;
        case MD_BLOCK_CODE:
            separateBlock();
            if (code_depth_ >= kMaximumNesting) {
                return fail();
            }
            ++code_depth_;
            ensureLinePrefix();
            break;
        case MD_BLOCK_P:
            if (list_item_depth_ == 0) {
                separateBlock();
            }
            ensureLinePrefix();
            break;
        case MD_BLOCK_HR: {
            separateBlock();
            ensureLinePrefix();
            std::size_t prefix_width = quote_depth_ > 0 ? quote_depth_ + 1 : 0;
            for (const auto &list : lists_) {
                appendRun(std::string(list.marker_width, ' '), {});
                prefix_width += list.marker_width;
            }

            constexpr std::size_t kDefaultRuleWidth = 40;
            constexpr std::size_t kMaximumRuleWidth = 160;
            std::size_t available = kDefaultRuleWidth;
            if (columns_.has_value()) {
                available = *columns_ > prefix_width ? *columns_ - prefix_width : 1;
            }
            available = std::min(available, kMaximumRuleWidth);
            std::string rule;
            for (std::size_t i = 0; i < available; ++i) {
                rule += utf8_ ? "─" : "-";
            }
            appendRun(std::move(rule), {});
            finishLine();
            break;
        }
        case MD_BLOCK_TABLE:
            separateBlock();
            table_rows_.clear();
            table_cell_count_ = 0;
            break;
        case MD_BLOCK_THEAD:
        case MD_BLOCK_TBODY:
            break;
        case MD_BLOCK_TR:
            table_rows_.emplace_back();
            break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            if (table_rows_.empty() || detail == nullptr ||
                ++table_cell_count_ > kMaximumTableCells)
                return fail();
            cell_alignment_ = static_cast<MD_BLOCK_TD_DETAIL *>(detail)->align;
            break;
        case MD_BLOCK_HTML:
            return fail();
        }
        return 0;
    }

    int leaveBlock(MD_BLOCKTYPE type) {
        switch (type) {
        case MD_BLOCK_DOC:
            break;
        case MD_BLOCK_QUOTE:
            if (!lineEmpty(current_)) {
                finishLine();
            }
            if (quote_depth_ == 0) {
                return fail();
            }
            --quote_depth_;
            break;
        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
            if (lists_.empty()) {
                return fail();
            }
            lists_.pop_back();
            break;
        case MD_BLOCK_LI:
            if (!lineEmpty(current_)) {
                finishLine();
            }
            if (list_item_depth_ == 0) {
                return fail();
            }
            --list_item_depth_;
            break;
        case MD_BLOCK_H:
            if (!lineEmpty(current_)) {
                finishLine();
            }
            if (heading_depth_ == 0) {
                return fail();
            }
            --heading_depth_;
            break;
        case MD_BLOCK_CODE:
            if (!lineEmpty(current_)) {
                finishLine();
            }
            if (code_depth_ == 0) {
                return fail();
            }
            --code_depth_;
            break;
        case MD_BLOCK_P:
            if (!lineEmpty(current_)) {
                finishLine();
            }
            break;
        case MD_BLOCK_TABLE:
            finishTable();
            break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            table_rows_.back().push_back({std::move(current_), cell_alignment_});
            current_ = {};
            break;
        case MD_BLOCK_HR:
        case MD_BLOCK_HTML:
        case MD_BLOCK_THEAD:
        case MD_BLOCK_TBODY:
        case MD_BLOCK_TR:
            break;
        }
        return 0;
    }

    int enterSpan(MD_SPANTYPE type, void *detail) {
        switch (type) {
        case MD_SPAN_EM:
            if (emphasis_depth_ >= kMaximumNesting) {
                return fail();
            }
            ++emphasis_depth_;
            break;
        case MD_SPAN_STRONG:
            if (strong_depth_ >= kMaximumNesting) {
                return fail();
            }
            ++strong_depth_;
            break;
        case MD_SPAN_CODE:
            if (inline_code_depth_ >= kMaximumNesting) {
                return fail();
            }
            ++inline_code_depth_;
            break;
        case MD_SPAN_DEL:
            if (strikethrough_depth_ >= kMaximumNesting) {
                return fail();
            }
            ++strikethrough_depth_;
            break;
        case MD_SPAN_A: {
            if (detail == nullptr || links_.size() >= kMaximumNesting) {
                return fail();
            }
            const auto &link = *static_cast<MD_SPAN_A_DETAIL *>(detail);
            links_.push_back({copyAttribute(link.href), link.is_autolink != 0});
            break;
        }
        case MD_SPAN_IMG: {
            if (detail == nullptr || links_.size() >= kMaximumNesting) {
                return fail();
            }
            const auto &image = *static_cast<MD_SPAN_IMG_DETAIL *>(detail);
            appendRun("Image: ", currentStyle());
            links_.push_back({copyAttribute(image.src), false, true});
            break;
        }
        case MD_SPAN_LATEXMATH:
        case MD_SPAN_LATEXMATH_DISPLAY:
        case MD_SPAN_WIKILINK:
        case MD_SPAN_U:
            return fail();
        }
        return 0;
    }

    int leaveSpan(MD_SPANTYPE type) {
        switch (type) {
        case MD_SPAN_EM:
            if (emphasis_depth_ == 0) {
                return fail();
            }
            --emphasis_depth_;
            break;
        case MD_SPAN_STRONG:
            if (strong_depth_ == 0) {
                return fail();
            }
            --strong_depth_;
            break;
        case MD_SPAN_CODE:
            if (inline_code_depth_ == 0) {
                return fail();
            }
            --inline_code_depth_;
            break;
        case MD_SPAN_DEL:
            if (strikethrough_depth_ == 0) {
                return fail();
            }
            --strikethrough_depth_;
            break;
        case MD_SPAN_A:
        case MD_SPAN_IMG:
            if (links_.empty()) {
                return fail();
            }
            if (!links_.back().autolink && !links_.back().destination.empty()) {
                appendRun(" (" + links_.back().destination + ")", currentStyle());
            }
            links_.pop_back();
            break;
        case MD_SPAN_LATEXMATH:
        case MD_SPAN_LATEXMATH_DISPLAY:
        case MD_SPAN_WIKILINK:
        case MD_SPAN_U:
            break;
        }
        return 0;
    }

    int text(MD_TEXTTYPE type, const char *text, MD_SIZE size) {
        if (text == nullptr && size != 0) {
            return fail();
        }
        const std::string_view value(text == nullptr ? "" : text, size);
        switch (type) {
        case MD_TEXT_NORMAL:
            appendText(value, false);
            break;
        case MD_TEXT_CODE:
            ensureLinePrefix();
            appendText(value, true);
            break;
        case MD_TEXT_NULLCHAR:
            appendRun(std::string(kReplacementCharacter), currentStyle());
            break;
        case MD_TEXT_BR:
            finishLine();
            ensureLinePrefix();
            break;
        case MD_TEXT_SOFTBR:
            appendRun(" ", currentStyle());
            break;
        case MD_TEXT_ENTITY:
            appendRun(decodeEntity(value), currentStyle());
            break;
        case MD_TEXT_HTML:
        case MD_TEXT_LATEXMATH:
            return fail();
        }
        return 0;
    }

    TerminalDocument finish() {
        if (!lineEmpty(current_)) {
            finishLine();
        }
        while (!document_.lines.empty() && lineEmpty(document_.lines.back())) {
            document_.lines.pop_back();
        }
        return std::move(document_);
    }

    bool failed() const noexcept { return failed_; }

  private:
    struct ListState {
        bool ordered = false;
        unsigned next = 1;
        std::size_t marker_width = 0;
    };

    struct LinkState {
        std::string destination;
        bool autolink = false;
        bool image = false;
    };

    int fail() noexcept {
        failed_ = true;
        return 1;
    }

    TerminalTextStyle currentStyle() const noexcept {
        TerminalTextStyle style;
        style.bold = strong_depth_ > 0 || heading_depth_ > 0;
        style.italic = emphasis_depth_ > 0;
        style.underline = !links_.empty() && !links_.back().image;
        style.code = inline_code_depth_ > 0 || code_depth_ > 0;
        style.strikethrough = strikethrough_depth_ > 0;
        style.accent = heading_depth_ > 0;
        return style;
    }

    void appendRun(std::string text, TerminalTextStyle style) {
        if (text.empty()) {
            return;
        }
        if (!current_.runs.empty()) {
            auto &previous = current_.runs.back();
            if (previous.style.bold == style.bold && previous.style.italic == style.italic &&
                previous.style.underline == style.underline && previous.style.code == style.code &&
                previous.style.strikethrough == style.strikethrough &&
                previous.style.accent == style.accent) {
                previous.text += text;
                return;
            }
        }
        current_.runs.push_back({std::move(text), style});
    }

    void appendText(std::string_view text, bool preserve_breaks) {
        std::size_t begin = 0;
        for (std::size_t index = 0; index < text.size(); ++index) {
            if (text[index] != '\n') {
                continue;
            }
            appendRun(std::string(text.substr(begin, index - begin)), currentStyle());
            finishLine();
            if (preserve_breaks && index + 1 < text.size()) {
                ensureLinePrefix();
            }
            begin = index + 1;
        }
        appendRun(std::string(text.substr(begin)), currentStyle());
    }

    void finishLine() {
        const std::string prefix = quotePrefix();
        if (prefix.empty() || !columns_.has_value() || *columns_ <= quote_depth_ + 1 ||
            code_depth_ > 0 || rendering_table_ || current_.runs.empty() ||
            current_.runs.front().text.compare(0, prefix.size(), prefix) != 0) {
            document_.lines.push_back(std::move(current_));
            current_ = {};
            return;
        }
        auto original = std::move(current_);
        current_ = {};
        addQuotePrefix();
        std::size_t line_width = quote_depth_ + 1;
        bool has_content = false;
        for (std::size_t i = 0; i < original.runs.size(); ++i) {
            const auto &run = original.runs[i];
            std::size_t offset = i == 0 ? prefix.size() : 0;
            while (offset < run.text.size()) {
                const auto cp = cli_detail::decodeCodePoint(run.text, offset);
                const std::size_t bytes = cp.valid ? cp.bytes : 1;
                const std::size_t width = cp.valid ? cli_detail::terminalCellWidth(cp.value) : 1;
                if (has_content && (line_width >= *columns_ || width > *columns_ - line_width)) {
                    document_.lines.push_back(std::move(current_));
                    current_ = {};
                    addQuotePrefix();
                    line_width = quote_depth_ + 1;
                    has_content = false;
                }
                appendRun(run.text.substr(offset, bytes), run.style);
                line_width += width;
                has_content = true;
                offset += bytes;
            }
        }
        document_.lines.push_back(std::move(current_));
        current_ = {};
    }

    void separateBlock() {
        if (!lineEmpty(current_)) {
            finishLine();
        }
        if (!document_.lines.empty() && !lineEmpty(document_.lines.back())) {
            TerminalLine separator;
            if (quote_depth_ > 0) {
                auto prefix = quotePrefix();
                prefix.pop_back();
                separator.runs.push_back({std::move(prefix), {}});
            }
            document_.lines.push_back(std::move(separator));
        }
    }

    void separateListIfNeeded() {
        if (lists_.empty() && list_item_depth_ == 0) {
            separateBlock();
        }
    }

    std::string quotePrefix() const {
        if (quote_depth_ == 0)
            return {};
        std::string prefix;
        for (std::size_t i = 0; i < quote_depth_; ++i)
            prefix += utf8_ ? "│" : "|";
        return prefix + " ";
    }

    void addQuotePrefix() { appendRun(quotePrefix(), {}); }

    void ensureLinePrefix() {
        if (lineEmpty(current_)) {
            addQuotePrefix();
        }
    }

    void appendListMarker(void *detail) {
        auto &list = lists_.back();
        std::string marker;
        if (list.ordered) {
            marker = std::to_string(list.next++) + ". ";
        } else {
            marker = "- ";
        }
        if (detail != nullptr) {
            const auto &item = *static_cast<MD_BLOCK_LI_DETAIL *>(detail);
            if (item.is_task != 0) {
                marker += (item.task_mark == 'x' || item.task_mark == 'X') ? "[x] " : "[ ] ";
            }
        }
        list.marker_width = marker.size();
        appendRun(std::move(marker), {});
    }

    struct TableCell {
        TerminalLine line;
        MD_ALIGN alignment;
    };

    void finishTable() {
        rendering_table_ = true;
        std::vector<std::size_t> widths;
        for (const auto &row : table_rows_) {
            if (widths.size() < row.size())
                widths.resize(row.size(), 0);
            for (std::size_t i = 0; i < row.size(); ++i) {
                std::size_t width = 0;
                for (const auto &run : row[i].line.runs)
                    width += cli_detail::textCellWidth(run.text);
                widths[i] = std::max(widths[i], width);
            }
        }
        std::size_t table_width = 1;
        for (auto width : widths)
            table_width += width + 3;
        std::size_t indent = 0;
        for (const auto &list : lists_)
            indent += list.marker_width;
        const std::size_t prefix_width = indent + (quote_depth_ ? quote_depth_ + 1 : 0);
        const bool compact =
            (columns_.has_value() &&
             (prefix_width >= *columns_ || table_width > *columns_ - prefix_width)) ||
            (!table_rows_.empty() && table_width > kMaximumMarkdownBytes / table_rows_.size());
        for (std::size_t r = 0; r < table_rows_.size(); ++r) {
            addQuotePrefix();
            appendRun(std::string(indent, ' '), {});
            appendRun("|", {});
            for (std::size_t i = 0; i < table_rows_[r].size(); ++i) {
                const auto &cell = table_rows_[r][i];
                std::size_t width = 0;
                for (const auto &run : cell.line.runs)
                    width += cli_detail::textCellWidth(run.text);
                const auto padding = compact ? 0 : widths[i] - width;
                const auto left = cell.alignment == MD_ALIGN_RIGHT    ? padding
                                  : cell.alignment == MD_ALIGN_CENTER ? padding / 2
                                                                      : 0;
                appendRun(" " + std::string(left, ' '), {});
                for (const auto &run : cell.line.runs)
                    appendRun(run.text, run.style);
                appendRun(std::string(padding - left, ' ') + " |", {});
            }
            finishLine();
            if (r == 0) {
                addQuotePrefix();
                appendRun(std::string(indent, ' '), {});
                appendRun("|", {});
                for (const auto width : widths)
                    appendRun(std::string(compact ? 3 : width + 2, '-') + "|", {});
                finishLine();
            }
        }
        table_rows_.clear();
        rendering_table_ = false;
    }

    std::vector<std::vector<TableCell>> table_rows_;
    MD_ALIGN cell_alignment_ = MD_ALIGN_DEFAULT;
    std::size_t table_cell_count_ = 0;
    std::optional<std::size_t> columns_;
    bool utf8_ = true;
    bool rendering_table_ = false;
    TerminalDocument document_;
    TerminalLine current_;
    std::vector<ListState> lists_;
    std::vector<LinkState> links_;
    std::size_t quote_depth_ = 0;
    std::size_t list_item_depth_ = 0;
    std::size_t heading_depth_ = 0;
    std::size_t code_depth_ = 0;
    std::size_t emphasis_depth_ = 0;
    std::size_t strong_depth_ = 0;
    std::size_t inline_code_depth_ = 0;
    std::size_t strikethrough_depth_ = 0;
    bool failed_ = false;
};

template <typename Callback> int protectCallback(void *userdata, Callback &&callback) noexcept {
    auto *builder = static_cast<MarkdownBuilder *>(userdata);
    if (builder == nullptr) {
        return 1;
    }
    try {
        return callback(*builder);
    } catch (...) {
        return 1;
    }
}

int enterBlock(MD_BLOCKTYPE type, void *detail, void *userdata) noexcept {
    return protectCallback(userdata, [type, detail](MarkdownBuilder &builder) {
        return builder.enterBlock(type, detail);
    });
}

int leaveBlock(MD_BLOCKTYPE type, void *, void *userdata) noexcept {
    return protectCallback(userdata,
                           [type](MarkdownBuilder &builder) { return builder.leaveBlock(type); });
}

int enterSpan(MD_SPANTYPE type, void *detail, void *userdata) noexcept {
    return protectCallback(userdata, [type, detail](MarkdownBuilder &builder) {
        return builder.enterSpan(type, detail);
    });
}

int leaveSpan(MD_SPANTYPE type, void *, void *userdata) noexcept {
    return protectCallback(userdata,
                           [type](MarkdownBuilder &builder) { return builder.leaveSpan(type); });
}

int renderText(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *userdata) noexcept {
    return protectCallback(userdata, [type, text, size](MarkdownBuilder &builder) {
        return builder.text(type, text, size);
    });
}

} /* namespace */

std::optional<TerminalDocument> parseMarkdown(std::string_view markdown,
                                              std::optional<std::size_t> columns, bool utf8) {
    if (markdown.size() > kMaximumMarkdownBytes ||
        markdown.size() > std::numeric_limits<MD_SIZE>::max()) {
        return std::nullopt;
    }

    MarkdownBuilder builder(columns, utf8);
    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MD_FLAG_NOHTMLBLOCKS | MD_FLAG_NOHTMLSPANS | MD_FLAG_PERMISSIVEURLAUTOLINKS |
                   MD_FLAG_PERMISSIVEEMAILAUTOLINKS | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS |
                   MD_FLAG_TABLES;
    parser.enter_block = enterBlock;
    parser.leave_block = leaveBlock;
    parser.enter_span = enterSpan;
    parser.leave_span = leaveSpan;
    parser.text = renderText;

    const int status = md_parse(markdown.empty() ? "" : markdown.data(),
                                static_cast<MD_SIZE>(markdown.size()), &parser, &builder);
    if (status != 0 || builder.failed()) {
        return std::nullopt;
    }
    return builder.finish();
}

} /* namespace octopus */
