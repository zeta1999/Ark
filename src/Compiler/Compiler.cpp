#include <Ark/Compiler/Compiler.hpp>

#include <Ark/Log.hpp>
#include <Ark/Lang/Lib.hpp>
#include <Ark/Function.hpp>

namespace Ark
{
    namespace Compiler
    {
        Compiler::Compiler()
        {}

        Compiler::~Compiler()
        {}

        void Compiler::feed(const std::string& code)
        {
            m_parser.feed(code);
            
            if (!m_parser.check())
            {
                Ark::Log::error("[Compiler] Program has errors");
                exit(1);
            }
        }

        void Compiler::compile()
        {
            /*
                Generating headers:
                    - lang name (to be sure we are executing an Ark file)
                        on 4 bytes (ark + padding)
                    - symbols table header
                        + elements
                    - values table header
                        + elements
            */
            m_bytecode.push_back('a');
            m_bytecode.push_back('r');
            m_bytecode.push_back('k');
            m_bytecode.push_back(Instruction::NOP);

            // symbols table
            m_bytecode.push_back(Instruction::SYM_TABLE_START);
                // gather symbols, values, and start to create code segments
                m_code_pages.emplace_back();  // create empty page
                _compile(m_parser.ast(), m_code_pages[0]);
            // push size
            pushNumber(static_cast<uint16_t>(m_symbols.size()));
            // push elements
            for (auto sym : m_symbols)
            {
                // push the string, nul terminated
                for (std::size_t i=0; i < sym.size(); ++i)
                    m_bytecode.push_back(sym[i]);
                m_bytecode.push_back(Instruction::NOP);
            }

            // values table
            m_bytecode.push_back(Instruction::VAL_TABLE_START);
            // push size
            pushNumber(static_cast<uint16_t>(m_values.size()));
            // push elements (separated with 0x00)
            for (auto val : m_values)
            {
                if (val.type == ValueType::Number)
                {
                    m_bytecode.push_back(Instruction::NUMBER_TYPE);
                    auto n = std::get<dozerg::HugeNumber>(val.value);
                    std::string t = n.toString(/* base */ 16);
                    for (std::size_t i=0; i < t.size(); ++i)
                        m_bytecode.push_back(t[i]);
                }
                else if (val.type == ValueType::String)
                {
                    m_bytecode.push_back(Instruction::STRING_TYPE);
                    std::string t = std::get<std::string>(val.value);
                    for (std::size_t i=0; i < t.size(); ++i)
                        m_bytecode.push_back(t[i]);
                }

                m_bytecode.push_back(Instruction::NOP);
            }

            // start code segments
            for (auto page : m_code_pages)
            {
                m_bytecode.push_back(CODE_SEGMENT_START);
                // push number of elements
                if (!page.size())
                {
                    pushNumber(0x00);
                    return;
                }
                pushNumber(static_cast<uint16_t>(page.size()));

                for (auto inst : page)
                {
                    // handle jump to code page (for functions call)
                    if (inst.jump_to_page == 0)
                        m_bytecode.push_back(inst.inst);
                    else
                        pushNumber(inst.jump_to_page);
                }
                // just in case we got too far, always add a HALT to be sure the
                // VM won't do anything crazy
                m_bytecode.push_back(Instruction::HALT);
            }

            if (!m_code_pages.size())
            {
                m_bytecode.push_back(Instruction::CODE_SEGMENT_START);
                pushNumber(0x00);
            }
        }

        void Compiler::_compile(Node x, std::vector<Inst>& page)
        {
            // register symbols
            if (x.nodeType() == NodeType::Symbol)
            {
                std::string name = x.getStringVal();
                std::size_t i = addSymbol(name);

                page.emplace_back(Instruction::LOAD_SYMBOL);
                pushNumber(static_cast<uint16_t>(i), &page);

                return;
            }
            // register values
            if (x.nodeType() == NodeType::String || x.nodeType() == NodeType::Number)
            {
                std::size_t i = addValue(x);

                page.emplace_back(Instruction::LOAD_CONST);
                pushNumber(static_cast<uint16_t>(i), &page);

                return;
            }
            // empty code block
            if (x.list().empty())
            {
                page.emplace_back(Instruction::NOP);
                return;
            }
            // registering structures
            if (x.list()[0].nodeType() == NodeType::Keyword)
            {
                Keyword n = x.list()[0].keyword();

                if (n == Keyword::If)
                {
                    // compile condition
                    _compile(x.list()[1], page);
                    // jump only if needed to the x.list()[2] part
                    page.emplace_back(Instruction::POP_JUMP_IF_TRUE);
                        // else code, generated in a temporary page
                        m_code_pages.emplace_back();
                        _compile(x.list()[3], m_code_pages.back());
                    // relative address to jump to if condition is true, casted as unsigned (don't worry, it's normal)
                    pushNumber(static_cast<uint16_t>(m_code_pages.back().size()), &page);
                    // adding temp page into current one, and removing temp page
                    for (auto&& inst : m_code_pages.back())
                        page.emplace_back(inst);
                    m_code_pages.pop_back();
                        // if code
                        _compile(x.list()[2], page);
                }
                else if (n == Keyword::Set)
                {
                    std::string name = x.list()[1].getStringVal();
                    std::size_t i = addSymbol(name);

                    // put value before symbol id
                    _compile(x.list()[2], page);

                    page.emplace_back(Instruction::STORE);
                    pushNumber(static_cast<uint16_t>(i), &page);
                }
                else if (n == Keyword::Def)
                {
                    std::string name = x.list()[1].getStringVal();
                    std::size_t i = addSymbol(name);

                    // put value before symbol id
                    _compile(x.list()[2], page);

                    page.emplace_back(Instruction::LET);
                    pushNumber(static_cast<uint16_t>(i), &page);
                }
                else if (n == Keyword::Fun)
                {
                    // TODO
                    /*x.setNodeType(NodeType::Lambda);
                    x.addEnv(env);
                    return x;*/
                }
                else if (n == Keyword::Begin)
                {
                    for (std::size_t i=1; i < x.list().size() - 1; ++i)
                        _compile(x.list()[i], page);
                    
                    // return last value
                    _compile(x.list()[x.list().size() - 1], page);
                    page.push_back(Instruction::RET);
                }
                else if (n == Keyword::While)
                {
                    // save current position to jump there at the end of the loop
                    std::size_t current = page.size();
                    // push condition
                    _compile(x.list()[1], page);
                    // push code to temp page
                        m_code_pages.emplace_back();
                        _compile(x.list()[2], m_code_pages.back());
                    // relative jump to end of block if condition is false
                    page.emplace_back(Instruction::POP_JUMP_IF_FALSE);
                    // relative address to jump to if condition is false, casted as unsigned (don't worry, it's normal)
                    pushNumber(static_cast<uint16_t>(m_code_pages.back().size()), &page);
                    // copy code from temp page and destroy temp page
                    for (auto&& inst : m_code_pages.back())
                        page.push_back(inst);
                    m_code_pages.pop_back();
                    // loop, jump to the condition
                    page.emplace_back(Instruction::JUMP);
                    // relative address casted as unsigned (don't worry, it's normal)
                    pushNumber(static_cast<uint16_t>(current), &page);
                }

                return;
            }

            /*Node proc(_execute(x.list()[0], env));
            Nodes exps;
            for (Node::Iterator exp=x.list().begin() + 1; exp != x.list().end(); ++exp)
                exps.push_back(_execute(*exp, env));

            if (proc.nodeType() == NodeType::Lambda)
                return _execute(proc.list()[2], new Environment(proc.list()[1].list(), exps, proc.getEnv()));
            else if (proc.nodeType() == NodeType::Proc)
                return proc.call(exps);
            else
            {
                Ark::Log::error("(Program) not a function");
                exit(1);
            }*/
        }

        std::size_t Compiler::addSymbol(const std::string& sym)
        {
            auto it = std::find(m_symbols.begin(), m_symbols.end(), sym);
            if (it == m_symbols.end())
            {
                m_symbols.push_back(sym);
                return m_symbols.size() - 1;
            }
            return (std::size_t) std::distance(m_symbols.begin(), it);
        }

        std::size_t Compiler::addValue(Node x)
        {
            Value v(x);
            auto it = std::find(m_values.begin(), m_values.end(), v);
            if (it == m_values.end())
            {
                m_values.push_back(v);
                return m_values.size() - 1;
            }
            return (std::size_t) std::distance(m_values.begin(), it);
        }

        void Compiler::pushNumber(uint16_t n, std::vector<Inst>* page)
        {
            if (page == nullptr)
            {
                m_bytecode.push_back((n & 0xff00) >> 8);
                m_bytecode.push_back(n & 0x00ff);
            }
            else
            {
                page->emplace_back((n & 0xff00) >> 8);
                page->emplace_back(n & 0x00ff);
            }
        }
    }
}