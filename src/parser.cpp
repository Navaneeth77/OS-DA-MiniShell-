// parser.cpp - turns a raw command line into a Pipeline.
//
// Stage 1 (tokenize): splits the line into words and operators while handling
//   'single quotes', "double quotes", backslash escapes, $VAR / ${VAR} / $? / $$
//   expansion, ~ expansion and # comments.
// Stage 2 (parse): groups tokens into commands separated by '|', attaches
//   redirections to their command and detects a trailing '&'.

#include "parser.h"
#include "shell.h"

#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <unistd.h>

namespace {

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class TokType { Word, Pipe, Amp, In, Out, Append, Dup };

struct Token {
    TokType type;
    std::string text;
    int fd = -1;
    int sourceFd = -1;
};

bool isNameStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool isNameChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Expands the variable reference starting at s[i] == '$', appending the value
// to `out`. Returns the index of the last character consumed.
size_t expandVariable(const std::string& s, size_t i, std::string& out) {
    size_t n = s.size();
    if (i + 1 >= n) { out += '$'; return i; }
    char c = s[i + 1];
    if (c == '?') { out += std::to_string(g_shell.lastStatus); return i + 1; }
    if (c == '$') { out += std::to_string(getpid()); return i + 1; }
    if (c == '{') {
        size_t close = s.find('}', i + 2);
        if (close == std::string::npos) throw ParseError("bad substitution: missing `}'");
        std::string name = s.substr(i + 2, close - i - 2);
        if (name.empty() || !isNameStart(name[0])) throw ParseError("${" + name + "}: bad substitution");
        if (const char* v = std::getenv(name.c_str())) out += v;
        return close;
    }
    if (isNameStart(c)) {
        size_t j = i + 1;
        while (j < n && isNameChar(s[j])) ++j;
        if (const char* v = std::getenv(s.substr(i + 1, j - i - 1).c_str())) out += v;
        return j - 1;
    }
    out += '$';
    return i;
}

std::vector<Token> tokenize(const std::string& line) {
    std::vector<Token> tokens;
    std::string cur;
    bool inWord = false;   // true once the current word has content (even an empty "")
    size_t n = line.size();

    auto flush = [&] {
        if (inWord) tokens.push_back({TokType::Word, cur});
        cur.clear();
        inWord = false;
    };

    for (size_t i = 0; i < n; ++i) {
        char c = line[i];

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { flush(); continue; }
        if (c == '#' && !inWord) break;

        if (c == '\\') {
            if (i + 1 < n) cur += line[++i];
            inWord = true;
            continue;
        }
        if (c == '\'') {
            size_t close = line.find('\'', i + 1);
            if (close == std::string::npos) throw ParseError("unexpected EOF while looking for matching `''");
            cur += line.substr(i + 1, close - i - 1);
            i = close;
            inWord = true;
            continue;
        }
        if (c == '"') {
            inWord = true;
            bool closed = false;
            for (++i; i < n; ++i) {
                char d = line[i];
                if (d == '"') { closed = true; break; }
                if (d == '\\' && i + 1 < n &&
                    (line[i + 1] == '"' || line[i + 1] == '\\' || line[i + 1] == '$' || line[i + 1] == '`')) {
                    cur += line[++i];
                    continue;
                }
                if (d == '$') { i = expandVariable(line, i, cur); continue; }
                cur += d;
            }
            if (!closed) throw ParseError("unexpected EOF while looking for matching `\"'");
            continue;
        }
        if (c == '$') {
            size_t before = cur.size();
            i = expandVariable(line, i, cur);
            if (cur.size() > before) inWord = true;   // unset vars vanish entirely
            continue;
        }
        if (c == '~' && !inWord && (i + 1 == n || line[i + 1] == '/' || std::isspace(static_cast<unsigned char>(line[i + 1])))) {
            const char* home = std::getenv("HOME");
            cur += home ? home : "~";
            inWord = true;
            continue;
        }

        // Operators that are planned for the final version.
        if ((c == '&' && i + 1 < n && line[i + 1] == '&') || (c == '|' && i + 1 < n && line[i + 1] == '|'))
            throw ParseError(std::string("`") + c + c + "' is not supported yet");
        if (c == ';') throw ParseError("`;' is not supported yet");

        if (c == '|') { flush(); tokens.push_back({TokType::Pipe, "|"}); continue; }
        if (c == '&') { flush(); tokens.push_back({TokType::Amp, "&"}); continue; }

        // Redirections: [n]< [n]> [n]>> [n]>&m
        bool digitRedirect = !inWord && std::isdigit(static_cast<unsigned char>(c)) && i + 1 < n &&
                             (line[i + 1] == '>' || line[i + 1] == '<');
        if (c == '<' || c == '>' || digitRedirect) {
            flush();
            Token t{TokType::Out, ""};
            if (digitRedirect) { t.fd = c - '0'; c = line[++i]; }
            if (c == '<') {
                t.type = TokType::In; t.text = "<";
                if (t.fd < 0) t.fd = 0;
            } else if (i + 1 < n && line[i + 1] == '>') {
                t.type = TokType::Append; t.text = ">>";
                ++i;
            } else if (i + 2 < n && line[i + 1] == '&' && std::isdigit(static_cast<unsigned char>(line[i + 2]))) {
                t.type = TokType::Dup; t.text = ">&";
                t.sourceFd = line[i + 2] - '0';
                i += 2;
            } else {
                t.type = TokType::Out; t.text = ">";
            }
            if (t.fd < 0) t.fd = 1;
            tokens.push_back(t);
            continue;
        }

        cur += c;
        inWord = true;
    }
    flush();
    return tokens;
}

}  // namespace

bool parseLine(const std::string& line, Pipeline& out, std::string& error) {
    out = Pipeline{};
    try {
        std::vector<Token> tokens = tokenize(line);
        Command cur;
        bool curHasRedirect = false;

        for (size_t i = 0; i < tokens.size(); ++i) {
            const Token& t = tokens[i];
            switch (t.type) {
            case TokType::Word:
                cur.argv.push_back(t.text);
                break;
            case TokType::Pipe:
                if (cur.argv.empty()) throw ParseError("syntax error near unexpected token `|'");
                out.commands.push_back(std::move(cur));
                cur = Command{};
                curHasRedirect = false;
                break;
            case TokType::Amp:
                if (i + 1 != tokens.size() || cur.argv.empty())
                    throw ParseError("syntax error near unexpected token `&'");
                out.background = true;
                break;
            case TokType::Dup: {
                Redirect r;
                r.kind = Redirect::Kind::Duplicate;
                r.fd = t.fd;
                r.sourceFd = t.sourceFd;
                cur.redirects.push_back(r);
                curHasRedirect = true;
                break;
            }
            default: {  // In / Out / Append need a file name
                if (i + 1 >= tokens.size())
                    throw ParseError("syntax error near unexpected token `newline'");
                if (tokens[i + 1].type != TokType::Word)
                    throw ParseError("syntax error near unexpected token `" + tokens[i + 1].text + "'");
                Redirect r;
                r.kind = t.type == TokType::In ? Redirect::Kind::Read
                       : t.type == TokType::Append ? Redirect::Kind::Append
                       : Redirect::Kind::Write;
                r.fd = t.fd;
                r.target = tokens[++i].text;
                cur.redirects.push_back(r);
                curHasRedirect = true;
                break;
            }
            }
        }

        if (!cur.argv.empty()) {
            out.commands.push_back(std::move(cur));
        } else if (!out.commands.empty()) {
            throw ParseError("syntax error: expected a command after `|'");
        } else if (curHasRedirect) {
            throw ParseError("syntax error: redirection without a command");
        }
    } catch (const ParseError& e) {
        error = e.what();
        out = Pipeline{};
        return false;
    }

    std::string text = trim(line);
    if (out.background && !text.empty() && text.back() == '&') text = trim(text.substr(0, text.size() - 1));
    out.text = text;
    return true;
}
