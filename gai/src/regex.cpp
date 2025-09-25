#include <stdexcept>
#include "regex.h"
#include "format.h"

namespace gai {
using namespace std::string_literals;

// Manages JIT resources. An instance of this will be created per thread.
struct JITContext {
  pcre2_match_context* match_context{nullptr};
  pcre2_jit_stack* jit_stack{nullptr};

  JITContext() {
    match_context = pcre2_match_context_create(nullptr);
    jit_stack = pcre2_jit_stack_create(32*1024, 512*1024, nullptr);
    pcre2_jit_stack_assign(match_context, nullptr, jit_stack);
  }

  ~JITContext() {
    if (match_context) pcre2_match_context_free(match_context);
    if (jit_stack) pcre2_jit_stack_free(jit_stack);
  }

  JITContext(const JITContext&) = delete;
  JITContext& operator=(const JITContext&) = delete;
};
thread_local JITContext thread_local_jit_context;

Pcre2Compiled::Pcre2Compiled(pcre2_code* p_, bool jitted_) : p{p_}, jitted{jitted_} {}

Pcre2Compiled::~Pcre2Compiled() {
  if (p) pcre2_code_free(p);
}

Pcre2Regex::Pcre2Regex(Pcre2Compiled&& re_, pcre2_match_data* m) : re{std::move(re_)}, match_data{m} {}

Pcre2Regex::~Pcre2Regex() {
  if (match_data) pcre2_match_data_free(match_data);
}

Pcre2Substitution::Pcre2Substitution(Pcre2Compiled&& re_, std::string_view sub_) : re{std::move(re_)},
                                                                                   substitute_pattern{sub_} {}

Pcre2Compiled Compile(std::string_view pattern, bool jit_compile, bool enable_utf) {
  int errornumber{0};
  PCRE2_SIZE erroroffset{0};

  uint32_t compile_options = 0;
  if (enable_utf) compile_options = PCRE2_UTF | PCRE2_UCP; // enable UTF-8 and Unicode property support

  Pcre2Compiled compiled{pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()),
                                       pattern.size(), compile_options, &errornumber, &erroroffset, nullptr),
                         false /* jitted */};
  if (compiled.p && jit_compile) {
    int jit_errorcode = pcre2_jit_compile(compiled.p, PCRE2_JIT_COMPLETE);
    if (jit_errorcode != 0) {
      std::string error;
      switch(jit_errorcode) {
        case PCRE2_ERROR_JIT_BADOPTION:
          error = "PCRE2 JIT compilation failed -- 'BADOPTION'\n"s;
          break;
        case PCRE2_ERROR_NOMEMORY:
          error = "PCRE2 JIT compilation failed -- cannot allocate memory\n"s;
          break;
        case PCRE2_ERROR_JIT_UNSUPPORTED:
          error = "PCRE2 JIT no supported on pattern\n"s;
          break;
        default:
          break;
      }
      throw std::runtime_error(error);
    }
    compiled.jitted = true;
  }
  if (!compiled.p) {
    std::string msg(256, '.');
    pcre2_get_error_message(errornumber, reinterpret_cast<PCRE2_UCHAR*>(&msg[1]), msg.size()-1);
    std::string_view error_msg = common::FormatIntoStringView<"PCRE2 compilation failed on pattern.\nPattern: %s\nError Offset: %d\nError: %s\n">(
                                                              pattern, erroroffset, msg);
    throw std::runtime_error(std::string(error_msg));
  }
  return compiled;
}

Pcre2Regex Regex(Pcre2Compiled&& pattern) {
  Pcre2Regex out(std::move(pattern), nullptr);
  out.match_data = pcre2_match_data_create_from_pattern(out.re.p, nullptr);
  return out;
}

bool Find(const Pcre2Regex& search_pattern, std::string_view content) {
  if (!search_pattern.re.p) return false;

  int retcode{0};
  if (!search_pattern.re.jitted) {
    retcode = pcre2_match(search_pattern.re.p,
                          reinterpret_cast<PCRE2_SPTR>(content.data()),
                          content.size(), 0, 0, search_pattern.match_data,
                          nullptr);
  } else {
    retcode = pcre2_jit_match(search_pattern.re.p,
                              reinterpret_cast<PCRE2_SPTR>(content.data()),
                              content.size(), 0, 0, search_pattern.match_data,
                              thread_local_jit_context.match_context);    
  }
  return retcode >= 0;
}

std::string_view Substitute(const Pcre2Substitution& substitution, std::string_view content,
                            std::string& scratch_buffer) {
  if (!substitution.re.p) {
    return content;
  }

  PCRE2_SIZE out_length = scratch_buffer.size();
  int rc = pcre2_substitute(substitution.re.p,
                            reinterpret_cast<PCRE2_SPTR>(content.data()),
                            content.size(),
                            0,
                            0,
                            nullptr,
                            nullptr,
                            reinterpret_cast<PCRE2_SPTR>(substitution.substitute_pattern.data()),
                            substitution.substitute_pattern.size(),
                            reinterpret_cast<PCRE2_UCHAR*>(scratch_buffer.data()),
                            &out_length);
  if (rc == 0) return content;

  if ((rc > 0) && out_length <= scratch_buffer.size()) {
    return {scratch_buffer.data(), out_length};
  } else {
    std::string_view error_msg = common::FormatIntoStringView<"Substitution requires more memory: needed %?, scratch size %zu\n">(
                                                              out_length, scratch_buffer.size());
    throw std::runtime_error(std::string(error_msg));
  }
  // no substitution performed
  return content;
}
 
} // namespace gai
