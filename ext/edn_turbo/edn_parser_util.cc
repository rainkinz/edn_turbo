#include <iostream>
#include <string>
#include <stack>
#include <vector>
#include <limits>
#include <exception>

#include <cstring>
#include <stdexcept>

#include <ruby/ruby.h>
#include <ruby/encoding.h>

#include "edn_parser.h"
#include "edn_parser_util.h"

namespace edn
{
    //
    // used to determine max number of chars in string value of a type
    template <typename T>
    static std::size_t get_max_chars(T)
    {
        std::stringstream s;
        s << std::fixed << std::numeric_limits<T>::max();
        return s.str().length();
    }

    static const std::size_t LL_max_chars = get_max_chars<>((long) 1);
    static const std::size_t LD_max_chars = get_max_chars<>((double) 1);


    // parser destructor
    //
    Parser::~Parser()
    {
        reset_state();
        del_top_meta_list();

        if (io_buffer) {
            free(reinterpret_cast<void*>(io_buffer));
        }
    }
    
    // =================================================================
    // for token-by-token parsing. If a discard or metadata is parsed,
    // attempt to get the following value
    //
    VALUE Parser::next()
    {
        VALUE token = EDNT_EOF_CONST;

        // buffer if reading from an IO
        if (core_io || (read_io != Qnil)) {
            fill_buf();
        }

        while (!is_eof())
        {
            // fetch a token. If it's metadata or discard
            VALUE v = EDNT_EOF_CONST;
            eTokenState state = parse_next(v);

            if (state == TOKEN_OK) {
                // valid token
                token = v;
                break;
            }
            else if (state == TOKEN_ERROR) {
                token = EDNT_EOF_CONST;
                break;
            }
        }

        return token;
    }

    // reset parsing state
    //
    void Parser::reset_state()
    {
        line_number = 1;
        discard.clear();

        // remove any remaining levels except for the first
        while (metadata.size() > 1) {
            del_top_meta_list();
        }
        // but clear any metadata on the first
        metadata.top()->clear();

        // clean up
        core_io = NULL;
        read_io = Qnil;
        p = pe = eof = NULL;
    }

    //
    // set a new source
    void Parser::set_source(const char* src, std::size_t len)
    {
        reset_state();
        // set ragel state
        p = src;
        pe = src + len;
        eof = pe;
    }

    void Parser::set_source(FILE* fp)
    {
        reset_state();
        core_io = fp;
    }

    void Parser::set_source(VALUE str_io)
    {
        reset_state();
        read_io = str_io;
    }

    //
    // for IO sources, read and fill a buffer
    void Parser::fill_buf()
    {
        std::string str_buf;

        // read as much data available
        if (core_io) {
            // ruby core IO types
            char c;
            while (1)
            {
                c = fgetc(core_io);
                if (c == EOF) {
                    break;
                }
                str_buf += c;
            }

        } else if (read_io != Qnil) {
            // StringIO, etc. Call read() from ruby side
            VALUE v = ruby_io_read(read_io);
            if (TYPE(v) == T_STRING) {
                str_buf.assign( StringValuePtr(v), RSTRING_LEN(v));
            }
        }

        // set the buffer to read from
        if (str_buf.length() > 0) {
            // first time when io_buffer is NULL, pe & p = 0
            uintmax_t new_length = (pe - p) + str_buf.length();
            if (new_length > (((uintmax_t) 1 << 32) - 1)) {
                // icu -> 32-bit. TODO: handle
                rb_raise(rb_eRuntimeError, "Unsupported string buffer length");
            }
            char* start = NULL;

            // allocate or extend storage needed
            if (!io_buffer) {
                io_buffer = reinterpret_cast<char*>(malloc(new_length));
                start = io_buffer;
            } else if (io_buffer_len < new_length) {
                // resize the buffer
                io_buffer = reinterpret_cast<char*>(realloc(reinterpret_cast<void*>(io_buffer), new_length));
            }

            if (!start) {
                // appending to the buffer but move the data not yet
                // parsed first to the front
                memmove(io_buffer, p, pe - p);
                start = io_buffer + (pe - p);
            }
            
            // and copy
            memcpy(start, str_buf.c_str(), str_buf.length()); 
            io_buffer_len = (uint32_t) new_length;

            // set ragel state
            p = io_buffer;
            pe = p + new_length;
            eof = pe;
        }
    }
    

    // =================================================================
    // work-around for idiotic rb_protect convention in order to avoid
    // using ruby/rice
    //
    typedef VALUE (edn_rb_f_type)( VALUE arg );

    // we're using at most 2 args
    struct prot_args {
        prot_args(VALUE r, ID m) :
            receiver(r), method(m), count(0) {
        }
        prot_args(VALUE r, ID m, VALUE arg) :
            receiver(r), method(m), count(1) {
            args[0] = arg;
        }
        prot_args(VALUE r, ID m, VALUE arg1, VALUE arg2) :
            receiver(r), method(m), count(2) {
            args[0] = arg1;
            args[1] = arg2;
        }

        VALUE call() const {
            return ((count == 0) ?
                    rb_funcall( receiver, method, 0 ) :
                    rb_funcall2( receiver, method, count, args ));
        }

    private:
        VALUE receiver;
        ID method;
        int count;
        VALUE args[2];
    };

    // this allows us to wrap with rb_protect()
    static inline VALUE edn_wrap_funcall2( VALUE arg )
    {
        const prot_args* a = reinterpret_cast<const prot_args*>(arg);
        if (a)
            return a->call();
        return Qnil;
    }

    static inline VALUE edn_prot_rb_funcall( edn_rb_f_type func, VALUE args )
    {
        int error;
        VALUE s = rb_protect( func, args, &error );
        if (error) Parser::throw_error(error);
        return s;
    }

    static inline VALUE edn_prot_rb_new_str(const char* str) {
        int error;
        VALUE s = rb_protect( reinterpret_cast<VALUE (*)(VALUE)>(rb_str_new_cstr),
                              reinterpret_cast<VALUE>(str), &error );
        if (error) Parser::throw_error(error);
        return s;
    }

    static inline VALUE edn_rb_enc_associate_utf8(VALUE str)
    {
        return rb_enc_associate(str, rb_utf8_encoding() );
    }

    // =================================================================
    // utils

    //
    // convert to int.. if string rep has more digits than long can
    // hold, call into ruby to get a big num
    VALUE Parser::integer_to_ruby(const char* str, std::size_t len)
    {
        if (str[len-1] == 'M' || len >= LL_max_chars)
        {
            std::string buf(str, len);
            VALUE vs = edn_prot_rb_new_str(buf.c_str());
            prot_args args(vs, EDNT_STRING_TO_I_METHOD);
            return edn_prot_rb_funcall( edn_wrap_funcall2, reinterpret_cast<VALUE>(&args) );
        }

        return LONG2NUM(buftotype<long>(str, len));
    }

    //
    // as above.. TODO: check exponential..
    VALUE Parser::float_to_ruby(const char* str, std::size_t len)
    {
        if (str[len-1] == 'M' || len >= LD_max_chars)
        {
            std::string buf(str, len);
            VALUE vs = edn_prot_rb_new_str(buf.c_str());

            if (str[len-1] == 'M') {
                return Parser::make_edn_type(EDNT_MAKE_BIG_DECIMAL_METHOD, vs);
            }

            prot_args args(vs, EDNT_STRING_TO_F_METHOD);
            return edn_prot_rb_funcall( edn_wrap_funcall2, reinterpret_cast<VALUE>(&args) );
        }

        return rb_float_new(buftotype<double>(str, len));
    }


    //
    // read from a StringIO - expensive!!!
    //
    VALUE Parser::ruby_io_read(VALUE io)
    {
        prot_args args(io, EDNT_READ_METHOD);
        return edn_prot_rb_funcall( edn_wrap_funcall2, reinterpret_cast<VALUE>(&args) );
    }

    //
    // copies the string data, unescaping any present values that need to be replaced
    //
    bool Parser::parse_byte_stream(const char *p_start, const char *p_end, VALUE& v_utf8,
                                   bool encode)
    {
        if (p_end > p_start) {
            std::string buf;

            if (encode) {
                if (!util::to_utf8(p_start, (uint32_t) (p_end - p_start), buf))
                    return false;
            }
            else {
                buf.append(p_start, p_end - p_start);
            }

            // utf-8 encode
            VALUE vs = edn_prot_rb_new_str(buf.c_str());
            int error;
            v_utf8 = rb_protect( edn_rb_enc_associate_utf8, vs, &error);
            if (error) Parser::throw_error(error);
            return true;
        } else if (p_end == p_start) {
            v_utf8 = rb_str_new("", 0);
            return true;
        }

        return false;
    }

    //
    // handles things like \c, \newline
    //
    bool Parser::parse_escaped_char(const char *p, const char *pe, VALUE& v)
    {
        std::string buf;
        std::size_t len = pe - p;
        buf.append(p, len);

        if (len > 1) {
            if      (buf == "newline") buf = '\n';
            else if (buf == "tab") buf = '\t';
            else if (buf == "return") buf = '\r';
            else if (buf == "space") buf = ' ';
            else if (buf == "formfeed") buf = '\f';
            else if (buf == "backspace") buf = '\b';
            // TODO: is this supported?
            else if (buf == "verticaltab") buf = '\v';
            else return false;
        }

        v = edn_prot_rb_new_str( buf.c_str() );
        return true;
    }


    //
    // get a set representation from the ruby side. See edn_turbo.rb
    VALUE Parser::make_edn_type(ID method, VALUE sym)
    {
        VALUE edn_module = rb_const_get(rb_cObject, edn::EDN_MODULE_SYMBOL);
        prot_args args(edn_module, method, sym);
        return edn_prot_rb_funcall( edn_wrap_funcall2, reinterpret_cast<VALUE>(&args) );
    }

    VALUE Parser::make_edn_type(ID method, VALUE name, VALUE data)
    {
        VALUE module = rb_const_get(rb_cObject, edn::EDN_MODULE_SYMBOL);
        return make_edn_type(module, method, name, data);
    }

    VALUE Parser::make_edn_type(VALUE module, ID method, VALUE name, VALUE data)
    {
        prot_args args(module, method, name, data);
        return edn_prot_rb_funcall( edn_wrap_funcall2, reinterpret_cast<VALUE>(&args) );
    }


    // =================================================================
    // METADATA
    //
    // returns an array of metadata value(s) saved in reverse order
    // (right to left) - the ruby side will interpret this
    VALUE Parser::ruby_meta()
    {
        VALUE m_ary = rb_ary_new();

        // pop from the back of the top-most list
        while (!metadata.top()->empty()) {
            rb_ary_push(m_ary, metadata.top()->back());
            metadata.top()->pop_back();
        }

        return m_ary;
    }


    // =================================================================
    //
    // error reporting
    void Parser::throw_error(int error)
    {
        if (error == 0)
            return;

        VALUE err = rb_errinfo();
        VALUE klass = rb_class_path(CLASS_OF(err));
        VALUE message = rb_obj_as_string(err);
        std::stringstream msg;
        msg << RSTRING_PTR(klass) << " exception: " << RSTRING_PTR(message);
        throw std::runtime_error(msg.str());
    }

    void Parser::error(const std::string& func, const std::string& err, char c) const
    {
        std::cerr << "Parse error "
            //            "from " << func << "() "
            ;
        if (err.length() > 0)
            std::cerr << "(" << err << ") ";
        if (c != '\0')
            std::cerr << "at '" << c << "' ";
        std::cerr << "on line " << line_number << std::endl;
    }
}
