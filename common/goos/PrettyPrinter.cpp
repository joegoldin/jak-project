/*!
 * @file PrettyPrinter.cpp
 * A Pretty Printer for GOOS.
 * It is not very good, but significantly better than putting everything on one line
 */

#include <cassert>
#include <stdexcept>
#include <utility>
#include <cstring>
#include "PrettyPrinter.h"
#include "Reader.h"
#include "third-party/fmt/core.h"

namespace pretty_print {

namespace {
// the integer representation is used here instead, wouldn't want really long numbers
const std::unordered_set<u32> banned_floats = {};
}  // namespace
/*!
 * Print a float in a nice representation if possibly, or an exact 32-bit integer constant to
 * be reinterpreted.
 */
goos::Object float_representation(float value) {
  u32 int_value;
  memcpy(&int_value, &value, 4);
  if (banned_floats.find(int_value) == banned_floats.end()) {
    return goos::Object::make_float(value);
  } else {
    return pretty_print::build_list("the-as", "float", fmt::format("#x{:x}", int_value));
  }
}

/*!
 * A single token which cannot be split between lines.
 */
struct FormToken {
  enum class TokenKind {
    WHITESPACE,
    STRING,
    OPEN_PAREN,
    DOT,
    CLOSE_PAREN,
    EMPTY_PAIR,
    SPECIAL_STRING  // has different alignment rules than STRING
  } kind;
  explicit FormToken(TokenKind _kind, std::string _str = "") : kind(_kind), str(std::move(_str)) {}

  std::string str;

  std::string toString() const {
    std::string s;
    switch (kind) {
      case TokenKind::WHITESPACE:
        s.push_back(' ');
        break;
      case TokenKind::STRING:
        s.append(str);
        break;
      case TokenKind::OPEN_PAREN:
        s.push_back('(');
        break;
      case TokenKind::DOT:
        s.push_back('.');
        break;
      case TokenKind::CLOSE_PAREN:
        s.push_back(')');
        break;
      case TokenKind::EMPTY_PAIR:
        s.append("()");
        break;
      case TokenKind::SPECIAL_STRING:
        s.append(str);
        break;
      default:
        throw std::runtime_error("toString unknown token kind");
    }
    return s;
  }
};

/*!
 * Convert a GOOS object to tokens and add it to the list.
 * This is the main function which recursively builds a list of tokens out of an s-expression.
 *
 * Note that not all GOOS objects can be pretty printed. Only the ones that can be directly
 * generated by the reader.
 */
void add_to_token_list(const goos::Object& obj, std::vector<FormToken>* tokens) {
  switch (obj.type) {
    case goos::ObjectType::EMPTY_LIST:
      tokens->emplace_back(FormToken::TokenKind::EMPTY_PAIR);
      break;
      // all of these can just be printed to a string and turned into a 'symbol'
    case goos::ObjectType::INTEGER:
    case goos::ObjectType::FLOAT:
    case goos::ObjectType::CHAR:
    case goos::ObjectType::SYMBOL:
    case goos::ObjectType::STRING:
      tokens->emplace_back(FormToken::TokenKind::STRING, obj.print());
      break;

      // it's important to break the pair up into smaller tokens which can then be split
      // across lines.
    case goos::ObjectType::PAIR: {
      tokens->emplace_back(FormToken::TokenKind::OPEN_PAREN);
      auto* to_print = &obj;
      for (;;) {
        if (to_print->is_pair()) {
          // first print the car into our token list:
          add_to_token_list(to_print->as_pair()->car, tokens);
          // then load up the cdr as the next thing to print
          to_print = &to_print->as_pair()->cdr;
          if (to_print->is_empty_list()) {
            // we're done, add a close paren and finish
            tokens->emplace_back(FormToken::TokenKind::CLOSE_PAREN);
            return;
          } else {
            // more to print, add whitespace
            tokens->emplace_back(FormToken::TokenKind::WHITESPACE);
          }
        } else {
          // got an improper list.
          // add a dot, space
          tokens->emplace_back(FormToken::TokenKind::DOT);
          tokens->emplace_back(FormToken::TokenKind::WHITESPACE);
          // then the thing and a close paren.
          add_to_token_list(*to_print, tokens);
          tokens->emplace_back(FormToken::TokenKind::CLOSE_PAREN);
          return;  // and we're done with this list.
        }
      }
    } break;

      // these are unsupported by the pretty printer.
    case goos::ObjectType::ARRAY:  // todo, we should probably handle arrays.
    case goos::ObjectType::LAMBDA:
    case goos::ObjectType::MACRO:
    case goos::ObjectType::ENVIRONMENT:
      throw std::runtime_error("tried to pretty print a goos object kind which is not supported.");
    default:
      assert(false);
  }
}

/*!
 * Linked list node representing a token in the output (whitespace, paren, newline, etc)
 */
struct PrettyPrinterNode {
  FormToken* tok = nullptr;  // if we aren't a newline, we will have a token.
  int line = -1;             // line that token occurs on. undef for newlines
  int lineIndent = -1;       // indent of line.  only valid for first token in the line
  int offset = -1;           // offset of beginning of token from left margin
  int specialIndentDelta = 0;
  bool is_line_separator = false;                      // true if line separator (not a token)
  PrettyPrinterNode *next = nullptr, *prev = nullptr;  // linked list
  PrettyPrinterNode* paren =
      nullptr;  // pointer to open paren if in parens.  open paren points to close and vice versa
  explicit PrettyPrinterNode(FormToken* _tok) { tok = _tok; }
  PrettyPrinterNode() = default;

  std::string debug_print() const {
    std::string result;
    if (tok) {
      result += fmt::format("tok: \"{}\"\n", tok->toString());
    }
    result += fmt::format("line: {}\nlineIn: {}\noffset: {}\nspecial: {}\nsep?: {}\n", line,
                          lineIndent, offset, specialIndentDelta, is_line_separator);
    return result;
  }
};

/*!
 * Container to track and cleanup all nodes after use.
 */
struct NodePool {
  std::vector<PrettyPrinterNode*> nodes;
  PrettyPrinterNode* allocate(FormToken* x) {
    auto result = new PrettyPrinterNode(x);
    nodes.push_back(result);
    return result;
  }

  PrettyPrinterNode* allocate() {
    auto result = new PrettyPrinterNode;
    nodes.push_back(result);
    return result;
  }

  NodePool() = default;

  ~NodePool() {
    for (auto& x : nodes) {
      delete x;
    }
  }

  // so we don't accidentally copy this.
  NodePool& operator=(const NodePool&) = delete;
  NodePool(const NodePool&) = delete;
};

/*!
 * Splice in a line break after the given node, it there isn't one already and if it isn't the last
 * node.
 */
void insertNewlineAfter(NodePool& pool, PrettyPrinterNode* node, int specialIndentDelta) {
  if (node->next && !node->next->is_line_separator) {
    auto* nl = pool.allocate();
    auto* next = node->next;
    node->next = nl;
    nl->prev = node;
    nl->next = next;
    next->prev = nl;
    nl->is_line_separator = true;
    nl->specialIndentDelta = specialIndentDelta;
  }
}

/*!
 * Splice in a line break before the given node, if there isn't one already and if it isn't the
 * first node.
 */
void insertNewlineBefore(NodePool& pool, PrettyPrinterNode* node, int specialIndentDelta) {
  if (node->prev && !node->prev->is_line_separator) {
    auto* nl = pool.allocate();
    auto* prev = node->prev;
    prev->next = nl;
    nl->prev = prev;
    nl->next = node;
    node->prev = nl;
    nl->is_line_separator = true;
    nl->specialIndentDelta = specialIndentDelta;
  }
}

/*!
 * Break a list across multiple lines. This is how line lengths are decreased.
 * This does not compute the proper indentation and leaves the list in a bad state.
 * After this has been called, the entire selection should be reformatted with propagate_pretty
 */
void breakList(NodePool& pool, PrettyPrinterNode* leftParen) {
  assert(!leftParen->is_line_separator);
  assert(leftParen->tok->kind == FormToken::TokenKind::OPEN_PAREN);
  auto* rp = leftParen->paren;
  assert(rp->tok->kind == FormToken::TokenKind::CLOSE_PAREN);

  for (auto* n = leftParen->next; n && n != rp; n = n->next) {
    if (!n->is_line_separator) {
      if (n->tok->kind == FormToken::TokenKind::OPEN_PAREN) {
        n = n->paren;
        assert(n->tok->kind == FormToken::TokenKind::CLOSE_PAREN);
        insertNewlineAfter(pool, n, 0);
      } else if (n->tok->kind != FormToken::TokenKind::WHITESPACE) {
        assert(n->tok->kind != FormToken::TokenKind::CLOSE_PAREN);
        insertNewlineAfter(pool, n, 0);
      }
    }
  }
}

/*!
 * Compute proper line numbers, offsets, and indents for a list of tokens with newlines
 * Will add newlines for close parens if needed.
 */
static PrettyPrinterNode* propagatePretty(NodePool& pool,
                                          PrettyPrinterNode* list,
                                          int line_length) {
  // propagate line numbers
  PrettyPrinterNode* rv = nullptr;
  int line = list->line;
  for (auto* n = list; n; n = n->next) {
    if (n->is_line_separator) {
      line++;
    } else {
      n->line = line;
      // add the weird newline.
      if (n->tok->kind == FormToken::TokenKind::CLOSE_PAREN) {
        if (n->line != n->paren->line) {
          if (n->prev && !n->prev->is_line_separator) {
            insertNewlineBefore(pool, n, 0);
            line++;
          }
          if (n->next && !n->next->is_line_separator) {
            insertNewlineAfter(pool, n, 0);
          }
        }
      }
    }
  }

  // compute offsets and indents
  std::vector<int> indentStack;
  indentStack.push_back(0);
  int offset = 0;
  PrettyPrinterNode* line_start = list;
  bool previous_line_sep = false;
  for (auto* n = list; n; n = n->next) {
    if (n->is_line_separator) {
      previous_line_sep = true;
      offset = indentStack.back() + n->specialIndentDelta;
    } else {
      if (previous_line_sep) {
        line_start = n;
        n->lineIndent = offset;
        previous_line_sep = false;
      }

      n->offset = offset;
      offset += n->tok->toString().length();
      if (offset > line_length && !rv)
        rv = line_start;
      if (n->tok->kind == FormToken::TokenKind::OPEN_PAREN) {
        if (!n->prev || n->prev->is_line_separator) {
          indentStack.push_back(offset + 1);
        } else {
          indentStack.push_back(offset - 1);
        }
      }

      if (n->tok->kind == FormToken::TokenKind::CLOSE_PAREN) {
        indentStack.pop_back();
      }
    }
  }
  return rv;
}

/*!
 * Get the token on the start of the next line. nullptr if we're the last line.
 */
PrettyPrinterNode* getNextLine(PrettyPrinterNode* start) {
  assert(!start->is_line_separator);
  int line = start->line;
  for (;;) {
    if (start->is_line_separator || start->line == line) {
      if (start->next)
        start = start->next;
      else
        return nullptr;
    } else {
      break;
    }
  }
  return start;
}

/*!
 * Get the next open paren on the current line (can start in the middle of line, not inclusive of
 * start) nullptr if there's no open parens on the rest of this line.
 */
PrettyPrinterNode* getNextListOnLine(PrettyPrinterNode* start) {
  int line = start->line;
  assert(!start->is_line_separator);
  if (!start->next || start->next->is_line_separator) {
    return nullptr;
  }

  start = start->next;
  while (!start->is_line_separator && start->line == line) {
    if (start->tok->kind == FormToken::TokenKind::OPEN_PAREN) {
      return start;
    }
    if (!start->next) {
      return nullptr;
    }
    start = start->next;
  }
  return nullptr;
}

PrettyPrinterNode* getNextOfKindOrStringOnLine(PrettyPrinterNode* start,
                                               FormToken::TokenKind kind) {
  int line = start->line;
  assert(!start->is_line_separator);
  if (!start->next || start->next->is_line_separator) {
    return nullptr;
  }

  start = start->next;
  while (!start->is_line_separator && start->line == line) {
    if (start->tok->kind == kind || start->tok->kind == FormToken::TokenKind::OPEN_PAREN) {
      return start;
    }
    if (!start->next) {
      return nullptr;
    }
    start = start->next;
  }
  return nullptr;
}

PrettyPrinterNode* getNextListOrEmptyListOnLine(PrettyPrinterNode* start) {
  int line = start->line;
  assert(!start->is_line_separator);
  if (!start->next || start->next->is_line_separator) {
    return nullptr;
  }

  start = start->next;
  while (!start->is_line_separator && start->line == line) {
    if (start->tok->kind == FormToken::TokenKind::OPEN_PAREN) {
      return start;
    }

    if (start->tok->kind == FormToken::TokenKind::EMPTY_PAIR) {
      return start;
    }

    if (!start->next) {
      return nullptr;
    }
    start = start->next;
  }
  return nullptr;
}

/*!
 * Get the first open paren on the current line (can start in the middle of line, inclusive of
 * start) nullptr if there's no open parens on the rest of this line
 */
PrettyPrinterNode* getFirstListOnLine(PrettyPrinterNode* start) {
  int line = start->line;
  assert(!start->is_line_separator);
  while (!start->is_line_separator && start->line == line) {
    if (start->tok->kind == FormToken::TokenKind::OPEN_PAREN)
      return start;
    if (!start->next)
      return nullptr;
    start = start->next;
  }
  return nullptr;
}

/*!
 * Get the first token on the first line which exceeds the max length
 */
PrettyPrinterNode* getFirstBadLine(PrettyPrinterNode* start, int line_length) {
  assert(!start->is_line_separator);
  int currentLine = start->line;
  auto* currentLineNode = start;
  for (;;) {
    if (start->is_line_separator) {
      assert(start->next);
      start = start->next;
    } else {
      if (start->line != currentLine) {
        currentLine = start->line;
        currentLineNode = start;
      }
      if (start->offset > line_length) {
        return currentLineNode;
      }
      if (!start->next) {
        return nullptr;
      }
      start = start->next;
    }
  }
}

/*!
 * Break insertion algorithm.
 */
void insertBreaksAsNeeded(NodePool& pool, PrettyPrinterNode* head, int line_length) {
  PrettyPrinterNode* last_line_complete = nullptr;
  PrettyPrinterNode* line_to_start_line_search = head;

  // loop over lines
  for (;;) {
    // compute lines as needed
    propagatePretty(pool, head, line_length);

    // search for a bad line starting at the last line we fixed
    PrettyPrinterNode* candidate_line = getFirstBadLine(line_to_start_line_search, line_length);
    // if we got the same line we started on, this means we couldn't fix it.
    if (candidate_line == last_line_complete) {
      candidate_line = nullptr;  // so we say our candidate was bad and try to find another
      PrettyPrinterNode* next_line = getNextLine(line_to_start_line_search);
      if (next_line) {
        candidate_line = getFirstBadLine(next_line, line_length);
      }
    }
    if (!candidate_line)
      break;

    // okay, we have a line which needs fixing.
    assert(!candidate_line->prev || candidate_line->prev->is_line_separator);
    PrettyPrinterNode* form_to_start = getFirstListOnLine(candidate_line);
    for (;;) {
      if (!form_to_start) {
        // this means we failed to hit the desired line length...
        break;
      }
      breakList(pool, form_to_start);
      propagatePretty(pool, head, line_length);
      if (getFirstBadLine(candidate_line, line_length) != candidate_line) {
        break;
      }

      form_to_start = getNextListOnLine(form_to_start);
      if (!form_to_start)
        break;
    }

    last_line_complete = candidate_line;
    line_to_start_line_search = candidate_line;
  }
}

/*!
 * Break a list across multiple lines. This is how line lengths are decreased.
 * This does not compute the proper indentation and leaves the list in a bad state.
 * After this has been called, the entire selection should be reformatted with propagate_pretty
 */
void breakList(NodePool& pool, PrettyPrinterNode* leftParen, PrettyPrinterNode* first_elt) {
  assert(!leftParen->is_line_separator);
  assert(leftParen->tok->kind == FormToken::TokenKind::OPEN_PAREN);
  auto* rp = leftParen->paren;
  assert(rp->tok->kind == FormToken::TokenKind::CLOSE_PAREN);

  bool breaking = false;
  for (auto* n = leftParen->next; n && n != rp; n = n->next) {
    if (n == first_elt) {
      breaking = true;
    }
    if (!n->is_line_separator) {
      if (n->tok->kind == FormToken::TokenKind::OPEN_PAREN) {
        n = n->paren;
        assert(n->tok->kind == FormToken::TokenKind::CLOSE_PAREN);
        if (breaking) {
          insertNewlineAfter(pool, n, 0);
        }

      } else if (n->tok->kind != FormToken::TokenKind::WHITESPACE) {
        assert(n->tok->kind != FormToken::TokenKind::CLOSE_PAREN);
        if (breaking) {
          insertNewlineAfter(pool, n, 0);
        }
      }
    }
  }
}

namespace {
const std::unordered_set<std::string> control_flow_start_forms = {
    "while", "dotimes", "until", "if", "when",
};
}

PrettyPrinterNode* seek_to_next_non_whitespace(PrettyPrinterNode* in) {
  in = in->next;
  while (in && (in->is_line_separator ||
                (in->tok && in->tok->kind == FormToken::TokenKind::WHITESPACE))) {
    in = in->next;
  }
  return in;
}

void insertSpecialBreaks(NodePool& pool, PrettyPrinterNode* node) {
  for (; node; node = node->next) {
    if (!node->is_line_separator && node->tok->kind == FormToken::TokenKind::STRING) {
      std::string& name = node->tok->str;
      if (name == "deftype") {  // todo!
        auto* parent_type_dec = getNextListOnLine(node);
        if (parent_type_dec) {
          insertNewlineAfter(pool, parent_type_dec->paren, 0);
        }
      }

      if (name == "begin") {
        breakList(pool, node->paren);
      }

      if (name == "defun" || name == "defmethod" || name == "defun-debug" || name == "let" ||
          name == "let*" || name == "rlet") {
        auto* first_list = getNextListOrEmptyListOnLine(node);
        if (first_list) {
          if (first_list->tok->kind == FormToken::TokenKind::EMPTY_PAIR) {
            insertNewlineAfter(pool, first_list, 0);
            breakList(pool, node->paren, first_list);
          } else {
            insertNewlineAfter(pool, first_list->paren, 0);
            breakList(pool, node->paren, first_list);
          }
        }

        if ((name == "let" || name == "let*" || name == "rlet") && first_list) {
          if (first_list->tok->kind == FormToken::TokenKind::OPEN_PAREN) {
            // we only want to break the variable list if it has multiple.
            bool single_var = false;
            // auto var_close_paren = first_list->paren;
            auto first_var_open = seek_to_next_non_whitespace(first_list);
            if (first_var_open->tok &&
                first_var_open->tok->kind == FormToken::TokenKind::OPEN_PAREN) {
              auto var_close_paren = first_var_open->paren;
              if (var_close_paren && var_close_paren->next) {
                auto iter = var_close_paren->next;
                while (iter &&
                       (iter->is_line_separator ||
                        (iter->tok && iter->tok->kind == FormToken::TokenKind::WHITESPACE))) {
                  iter = iter->next;
                }
                if (iter) {
                  if (iter->tok && iter->tok->kind == FormToken::TokenKind::CLOSE_PAREN) {
                    single_var = true;
                  }
                }
              }
            }

            if (!single_var) {
              breakList(pool, first_list);
            }
          }
          auto open_paren = node->prev;
          if (open_paren && open_paren->tok->kind == FormToken::TokenKind::OPEN_PAREN) {
            // insertNewlineBefore(pool, open_paren, 0);
            if (open_paren->prev) {
              breakList(pool, open_paren->prev->paren, open_paren);
            }
          }
        }
      }

      if (control_flow_start_forms.find(name) != control_flow_start_forms.end()) {
        auto* parent_type_dec = getNextOfKindOrStringOnLine(node, FormToken::TokenKind::STRING);

        if (parent_type_dec) {
          if (parent_type_dec->tok->kind == FormToken::TokenKind::OPEN_PAREN) {
            insertNewlineAfter(pool, parent_type_dec->paren, 0);
            breakList(pool, node->paren, parent_type_dec);
          } else {
            insertNewlineAfter(pool, parent_type_dec, 0);
            breakList(pool, node->paren, parent_type_dec);
          }

          auto open_paren = node->prev;
          if (open_paren && open_paren->tok->kind == FormToken::TokenKind::OPEN_PAREN) {
            if (open_paren->prev && !open_paren->prev->is_line_separator) {
              if (open_paren->prev) {
                auto to_break = open_paren->prev->paren;
                if (to_break->tok && to_break->tok->kind == FormToken::TokenKind::OPEN_PAREN) {
                  breakList(pool, open_paren->prev->paren, open_paren);
                }
              }
            }
          }
        }
      }

      if (name == "cond") {
        auto* start_of_case = getNextListOnLine(node);
        while (true) {
          // let's break this case:
          assert(start_of_case->tok->kind == FormToken::TokenKind::OPEN_PAREN);
          auto end_of_case = start_of_case->paren;
          assert(end_of_case->tok->kind == FormToken::TokenKind::CLOSE_PAREN);
          // get the first thing in the case

          // and break there.
          breakList(pool, start_of_case);
          // now, look for the next case.
          auto next = end_of_case->next;
          while (next &&
                 (next->is_line_separator || next->tok->kind == FormToken::TokenKind::WHITESPACE)) {
            next = next->next;
          }
          if (!next) {
            break;
          }
          if (next->tok->kind == FormToken::TokenKind::CLOSE_PAREN) {
            break;
          }
          if (next->tok->kind != FormToken::TokenKind::OPEN_PAREN) {
            break;
          }
          start_of_case = next;
        }

        // break cond into a multi-line always
        breakList(pool, node->paren);
      }
    }
  }
}

std::string to_string(const goos::Object& obj, int line_length) {
  NodePool pool;
  std::vector<FormToken> tokens;
  add_to_token_list(obj, &tokens);
  assert(!tokens.empty());
  std::string pretty;

  // build linked list of nodes
  PrettyPrinterNode* head = pool.allocate(&tokens[0]);
  PrettyPrinterNode* node = head;
  head->line = 0;
  head->offset = 0;
  head->lineIndent = 0;
  int offset = head->tok->toString().length();
  for (size_t i = 1; i < tokens.size(); i++) {
    node->next = pool.allocate(&tokens[i]);
    node->next->prev = node;
    node = node->next;
    node->line = 0;
    node->offset = offset;
    offset += node->tok->toString().length();
    node->lineIndent = 0;
  }

  // attach parens.
  std::vector<PrettyPrinterNode*> parenStack;
  parenStack.push_back(nullptr);
  for (PrettyPrinterNode* n = head; n; n = n->next) {
    if (n->tok->kind == FormToken::TokenKind::OPEN_PAREN) {
      parenStack.push_back(n);
    } else if (n->tok->kind == FormToken::TokenKind::CLOSE_PAREN) {
      n->paren = parenStack.back();
      parenStack.back()->paren = n;
      parenStack.pop_back();
    } else {
      n->paren = parenStack.back();
    }
  }
  assert(parenStack.size() == 1);
  assert(!parenStack.back());

  insertSpecialBreaks(pool, head);
  propagatePretty(pool, head, line_length);
  insertBreaksAsNeeded(pool, head, line_length);

  // write to string
  bool newline_prev = true;
  for (PrettyPrinterNode* n = head; n; n = n->next) {
    if (n->is_line_separator) {
      pretty.push_back('\n');
      newline_prev = true;
    } else {
      if (newline_prev) {
        pretty.append(n->lineIndent, ' ');
        newline_prev = false;
        if (n->tok->kind == FormToken::TokenKind::WHITESPACE)
          continue;
      }
      pretty.append(n->tok->toString());
    }
  }

  return pretty;
}

goos::Reader pretty_printer_reader;

goos::Reader& get_pretty_printer_reader() {
  return pretty_printer_reader;
}

goos::Object to_symbol(const std::string& str) {
  return goos::SymbolObject::make_new(pretty_printer_reader.symbolTable, str);
}

goos::Object build_list(const std::string& str) {
  return build_list(to_symbol(str));
}

goos::Object build_list(const goos::Object& obj) {
  return goos::PairObject::make_new(obj, goos::EmptyListObject::make_new());
}

goos::Object build_list(const std::vector<goos::Object>& objects) {
  if (objects.empty()) {
    return goos::EmptyListObject::make_new();
  } else {
    return build_list(objects.data(), objects.size());
  }
}

// build a list out of an array of forms
goos::Object build_list(const goos::Object* objects, int count) {
  assert(count);
  auto car = objects[0];
  goos::Object cdr;
  if (count - 1) {
    cdr = build_list(objects + 1, count - 1);
  } else {
    cdr = goos::EmptyListObject::make_new();
  }
  return goos::PairObject::make_new(car, cdr);
}

// build a list out of a vector of strings that are converted to symbols
goos::Object build_list(const std::vector<std::string>& symbols) {
  if (symbols.empty()) {
    return goos::EmptyListObject::make_new();
  }
  std::vector<goos::Object> f;
  f.reserve(symbols.size());
  for (auto& x : symbols) {
    f.push_back(to_symbol(x));
  }
  return build_list(f.data(), f.size());
}

void append(goos::Object& _in, const goos::Object& add) {
  auto* in = &_in;
  while (in->is_pair() && !in->as_pair()->cdr.is_empty_list()) {
    in = &in->as_pair()->cdr;
  }

  if (!in->is_pair()) {
    assert(false);  // invalid list
  }
  in->as_pair()->cdr = add;
}
}  // namespace pretty_print
