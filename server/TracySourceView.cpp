#include <inttypes.h>
#include <stdio.h>

#include <capstone/capstone.h>

#include "../imgui/imgui.h"
#include "TracyColor.hpp"
#include "TracyFilesystem.hpp"
#include "TracyImGui.hpp"
#include "TracyPrint.hpp"
#include "TracySort.hpp"
#include "TracySourceView.hpp"
#include "TracyWorker.hpp"

#include "IconsFontAwesome5.h"

namespace tracy
{

enum { JumpSeparation = 6 };
enum { JumpArrowBase = 9 };

SourceView::SourceView( ImFont* font )
    : m_font( font )
    , m_file( nullptr )
    , m_fileStringIdx( 0 )
    , m_symAddr( 0 )
    , m_targetAddr( 0 )
    , m_data( nullptr )
    , m_dataSize( 0 )
    , m_targetLine( 0 )
    , m_selectedLine( 0 )
    , m_hoveredLine( 0 )
    , m_hoveredSource( 0 )
    , m_codeLen( 0 )
    , m_highlightAddr( 0 )
    , m_asmRelative( false )
    , m_asmShowSourceLocation( true )
    , m_calcInlineStats( true )
    , m_showJumps( true )
{
}

SourceView::~SourceView()
{
    delete[] m_data;
}

void SourceView::OpenSource( const char* fileName, int line )
{
    m_targetLine = line;
    m_selectedLine = line;
    m_targetAddr = 0;
    m_baseAddr = 0;
    m_symAddr = 0;
    m_sourceFiles.clear();

    ParseSource( fileName, nullptr );
    assert( !m_lines.empty() );
}

void SourceView::OpenSymbol( const char* fileName, int line, uint64_t baseAddr, uint64_t symAddr, const Worker& worker )
{
    m_targetLine = line;
    m_targetAddr = symAddr;
    m_baseAddr = baseAddr;
    m_symAddr = symAddr;
    m_sourceFiles.clear();
    m_selectedAddresses.clear();
    m_selectedAddresses.emplace( symAddr );

    ParseSource( fileName, &worker );
    Disassemble( baseAddr, worker );
    SelectLine( line, &worker, true, symAddr );

    if( !m_lines.empty() )
    {
        if( !m_asm.empty() )
        {
            m_displayMode = DisplayMixed;
        }
        else
        {
            m_displayMode = DisplaySource;
        }
    }
    else
    {
        assert( !m_asm.empty() );
        m_displayMode = DisplayAsm;
    }
}

void SourceView::ParseSource( const char* fileName, const Worker* worker )
{
    if( m_file != fileName )
    {
        m_file = fileName;
        m_fileStringIdx = worker ? worker->FindStringIdx( fileName ) : 0;
        m_lines.clear();
        if( fileName )
        {
            FILE* f = fopen( fileName, "rb" );
            fseek( f, 0, SEEK_END );
            const auto sz = ftell( f );
            fseek( f, 0, SEEK_SET );
            if( sz > m_dataSize )
            {
                delete[] m_data;
                m_data = new char[sz+1];
                m_dataSize = sz;
            }
            fread( m_data, 1, sz, f );
            m_data[sz] = '\0';
            fclose( f );

            auto txt = m_data;
            for(;;)
            {
                auto end = txt;
                while( *end != '\n' && *end != '\r' && end - m_data < sz ) end++;
                m_lines.emplace_back( Line { txt, end } );
                if( *end == '\n' )
                {
                    end++;
                    if( *end == '\r' ) end++;
                }
                else if( *end == '\r' )
                {
                    end++;
                    if( *end == '\n' ) end++;
                }
                if( *end == '\0' ) break;
                txt = end;
            }
        }
    }
}

bool SourceView::Disassemble( uint64_t symAddr, const Worker& worker )
{
    m_asm.clear();
    m_jumpTable.clear();
    m_jumpOut.clear();
    m_maxJumpLevel = 0;
    if( symAddr == 0 ) return false;
    const auto arch = worker.GetCpuArch();
    if( arch == CpuArchUnknown ) return false;
    uint32_t len;
    auto code = worker.GetSymbolCode( symAddr, len );
    if( !code ) return false;
    csh handle;
    cs_err rval = CS_ERR_ARCH;
    switch( arch )
    {
    case CpuArchX86:
        rval = cs_open( CS_ARCH_X86, CS_MODE_32, &handle );
        break;
    case CpuArchX64:
        rval = cs_open( CS_ARCH_X86, CS_MODE_64, &handle );
        break;
    case CpuArchArm32:
        rval = cs_open( CS_ARCH_ARM, CS_MODE_ARM, &handle );
        break;
    case CpuArchArm64:
        rval = cs_open( CS_ARCH_ARM64, CS_MODE_ARM, &handle );
        break;
    default:
        assert( false );
        break;
    }
    if( rval != CS_ERR_OK ) return false;
    cs_option( handle, CS_OPT_DETAIL, CS_OPT_ON );
    cs_insn* insn;
    size_t cnt = cs_disasm( handle, (const uint8_t*)code, len, symAddr, 0, &insn );
    if( cnt > 0 )
    {
        int mLenMax = 0;
        m_asm.reserve( cnt );
        for( size_t i=0; i<cnt; i++ )
        {
            const auto& op = insn[i];
            const auto& detail = *op.detail;
            bool hasJump = false;
            for( auto j=0; j<detail.groups_count; j++ )
            {
                if( detail.groups[j] == CS_GRP_JUMP || detail.groups[j] == CS_GRP_CALL || detail.groups[j] == CS_GRP_RET )
                {
                    hasJump = true;
                    break;
                }
            }
            uint64_t jumpAddr = 0;
            if( hasJump )
            {
                switch( arch )
                {
                case CpuArchX86:
                case CpuArchX64:
                    if( detail.x86.op_count == 1 && detail.x86.operands[0].type == X86_OP_IMM )
                    {
                        jumpAddr = (uint64_t)detail.x86.operands[0].imm;
                    }
                    break;
                case CpuArchArm32:
                    if( detail.arm.op_count == 1 && detail.arm.operands[0].type == ARM_OP_IMM )
                    {
                        jumpAddr = (uint64_t)detail.arm.operands[0].imm;
                    }
                    break;
                case CpuArchArm64:
                    if( detail.arm64.op_count == 1 && detail.arm64.operands[0].type == ARM64_OP_IMM )
                    {
                        jumpAddr = (uint64_t)detail.arm64.operands[0].imm;
                    }
                    break;
                default:
                    assert( false );
                    break;
                }
                if( jumpAddr >= symAddr && jumpAddr < symAddr + len )
                {
                    const auto min = std::min( jumpAddr, op.address );
                    const auto max = std::max( jumpAddr, op.address );
                    auto it = m_jumpTable.find( jumpAddr );
                    if( it == m_jumpTable.end() )
                    {
                        m_jumpTable.emplace( jumpAddr, JumpData { min, max, 0, { op.address } } );
                    }
                    else
                    {
                        if( it->second.min > min ) it->second.min = min;
                        else if( it->second.max < max ) it->second.max = max;
                        it->second.source.emplace_back( op.address );
                    }
                }
                else
                {
                    m_jumpOut.emplace( op.address );
                }
            }
            m_asm.emplace_back( AsmLine { op.address, jumpAddr, op.mnemonic, op.op_str } );
            const auto mLen = strlen( op.mnemonic );
            if( mLen > mLenMax ) mLenMax = mLen;

            uint32_t mLineMax = 0;
            uint32_t srcline;
            const auto srcidx = worker.GetLocationForAddress( op.address, srcline );
            if( srcline != 0 )
            {
                if( srcline > mLineMax ) mLineMax = srcline;
                const auto idx = srcidx.Idx();
                auto sit = m_sourceFiles.find( idx );
                if( sit == m_sourceFiles.end() )
                {
                    m_sourceFiles.emplace( idx, srcline );
                }
            }
            char tmp[16];
            sprintf( tmp, "%" PRIu32, mLineMax );
            m_maxLine = strlen( tmp ) + 1;
        }
        cs_free( insn, cnt );
        m_maxMnemonicLen = mLenMax + 2;
        if( !m_jumpTable.empty() )
        {
            struct JumpRange
            {
                uint64_t target;
                uint64_t len;
            };
            std::vector<JumpRange> jumpRange;
            jumpRange.reserve( m_jumpTable.size() );
            for( auto& v : m_jumpTable )
            {
                pdqsort_branchless( v.second.source.begin(), v.second.source.end() );
                jumpRange.emplace_back( JumpRange { v.first, v.second.max - v.second.min } );
            }
            pdqsort_branchless( jumpRange.begin(), jumpRange.end(), []( const auto& l, const auto& r ) { return l.len < r.len; } );
            std::vector<std::vector<std::pair<uint64_t, uint64_t>>> levelRanges;
            for( auto& v : jumpRange )
            {
                auto it = m_jumpTable.find( v.target );
                assert( it != m_jumpTable.end() );
                int level = 0;
                for(;;)
                {
                    assert( levelRanges.size() >= level );
                    if( levelRanges.size() == level )
                    {
                        it->second.level = level;
                        levelRanges.push_back( { { it->second.min, it->second.max } } );
                        break;
                    }
                    else
                    {
                        bool validFit = true;
                        auto& lr = levelRanges[level];
                        for( auto& range : lr )
                        {
                            assert( !( it->second.min >= range.first && it->second.max <= range.second ) );
                            if( it->second.min <= range.second && it->second.max >= range.first )
                            {
                                validFit = false;
                                break;
                            }
                        }
                        if( validFit )
                        {
                            it->second.level = level;
                            lr.emplace_back( it->second.min, it->second.max );
                            break;
                        }
                        level++;
                    }
                }
                if( level > m_maxJumpLevel ) m_maxJumpLevel = level;
            }
        }
    }
    cs_close( &handle );
    m_codeLen = len;
    return true;
}

void SourceView::Render( const Worker& worker )
{
    m_highlightAddr.Decay( 0 );
    m_hoveredLine.Decay( 0 );
    m_hoveredSource.Decay( 0 );

    if( m_symAddr == 0 )
    {
        if( m_file ) TextFocused( ICON_FA_FILE " File:", m_file );
        TextColoredUnformatted( ImVec4( 1.f, 1.f, 0.2f, 1.f ), ICON_FA_EXCLAMATION_TRIANGLE );
        ImGui::SameLine();
        TextColoredUnformatted( ImVec4( 1.f, 0.3f, 0.3f, 1.f ), "The source file contents might not reflect the actual profiled code!" );
        ImGui::SameLine();
        TextColoredUnformatted( ImVec4( 1.f, 1.f, 0.2f, 1.f ), ICON_FA_EXCLAMATION_TRIANGLE );

        RenderSimpleSourceView();
    }
    else
    {
        RenderSymbolView( worker );
    }
}

void SourceView::RenderSimpleSourceView()
{
    ImGui::BeginChild( "##sourceView", ImVec2( 0, 0 ), true );
    if( m_font ) ImGui::PushFont( m_font );
    if( m_targetLine != 0 )
    {
        int lineNum = 1;
        for( auto& line : m_lines )
        {
            if( m_targetLine == lineNum )
            {
                m_targetLine = 0;
                ImGui::SetScrollHereY();
            }
            RenderLine( line, lineNum++, 0, 0, nullptr );
        }
    }
    else
    {
        ImGuiListClipper clipper( (int)m_lines.size() );
        while( clipper.Step() )
        {
            for( auto i=clipper.DisplayStart; i<clipper.DisplayEnd; i++ )
            {
                RenderLine( m_lines[i], i+1, 0, 0, nullptr );
            }
        }
    }
    if( m_font ) ImGui::PopFont();
    ImGui::EndChild();
}

void SourceView::RenderSymbolView( const Worker& worker )
{
    assert( m_symAddr != 0 );

    auto sym = worker.GetSymbolData( m_symAddr );
    assert( sym );
    if( sym->isInline )
    {
        auto parent = worker.GetSymbolData( m_baseAddr );
        if( parent )
        {
            TextFocused( ICON_FA_PUZZLE_PIECE " Symbol:", worker.GetString( parent->name ) );
        }
        else
        {
            char tmp[16];
            sprintf( tmp, "0x%x", m_baseAddr );
            TextFocused( ICON_FA_PUZZLE_PIECE " Symbol:", tmp );
        }
    }
    else
    {
        TextFocused( ICON_FA_PUZZLE_PIECE " Symbol:", worker.GetString( sym->name ) );
    }

    auto inlineList = worker.GetInlineSymbolList( m_baseAddr, m_codeLen );
    if( inlineList )
    {
        const auto symEnd = m_baseAddr + m_codeLen;
        Vector<uint64_t> symInline( m_baseAddr );
        while( *inlineList < symEnd )
        {
            if( *inlineList != m_baseAddr )
            {
                symInline.push_back_non_empty( *inlineList );
            }
            inlineList++;
        }

        SmallCheckbox( ICON_FA_SITEMAP " Function:", &m_calcInlineStats );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( -1 );
        ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
        if( ImGui::BeginCombo( "##functionList", worker.GetString( sym->name ), ImGuiComboFlags_HeightLarge ) )
        {
            for( auto& v : symInline )
            {
                auto isym = worker.GetSymbolData( v );
                assert( isym );
                ImGui::PushID( v );
                if( ImGui::Selectable( worker.GetString( isym->name ), v == m_symAddr ) )
                {
                    m_symAddr = v;
                }
                ImGui::PopID();
                ImGui::SameLine();
                char tmp[32];
                sprintf( tmp, "(0x%x)", v );
                TextDisabledUnformatted( tmp );
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleVar();
    }

    TextDisabledUnformatted( "Mode:" );
    ImGui::SameLine();
    ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
    if( !m_lines.empty() )
    {
        ImGui::RadioButton( "Source", &m_displayMode, DisplaySource );
        if( !m_asm.empty() )
        {
            ImGui::SameLine();
            ImGui::RadioButton( "Assembly", &m_displayMode, DisplayAsm );
            ImGui::SameLine();
            ImGui::RadioButton( "Combined", &m_displayMode, DisplayMixed );
        }
    }
    else
    {
        ImGui::RadioButton( "Assembly", &m_displayMode, DisplayAsm );
    }
    ImGui::PopStyleVar();

    if( !m_asm.empty() )
    {
        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        TextFocused( ICON_FA_WEIGHT_HANGING " Code size:", MemSizeToString( m_codeLen ) );
    }

    uint32_t iptotalSrc = 0, iptotalAsm = 0;
    unordered_flat_map<uint64_t, uint32_t> ipcountSrc, ipcountAsm;
    if( m_calcInlineStats )
    {
        GatherIpStats( m_symAddr, iptotalSrc, iptotalAsm, ipcountSrc, ipcountAsm, worker );
    }
    else
    {
        GatherIpStats( m_baseAddr, iptotalSrc, iptotalAsm, ipcountSrc, ipcountAsm, worker );
        auto iptr = worker.GetInlineSymbolList( m_baseAddr, m_codeLen );
        if( iptr )
        {
            const auto symEnd = m_baseAddr + m_codeLen;
            while( *iptr < symEnd )
            {
                GatherIpStats( *iptr, iptotalSrc, iptotalAsm, ipcountSrc, ipcountAsm, worker );
                iptr++;
            }
        }
        iptotalSrc = iptotalAsm;
    }
    if( iptotalAsm > 0 )
    {
        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();
        TextFocused( ICON_FA_EYE_DROPPER " Samples:", RealToString( iptotalAsm ) );
    }

    ImGui::Separator();

    uint64_t jumpOut = 0;
    switch( m_displayMode )
    {
    case DisplaySource:
        RenderSymbolSourceView( iptotalSrc, ipcountSrc, worker );
        break;
    case DisplayAsm:
        jumpOut = RenderSymbolAsmView( iptotalAsm, ipcountAsm, worker );
        break;
    case DisplayMixed:
        ImGui::Columns( 2 );
        RenderSymbolSourceView( iptotalSrc, ipcountSrc, worker );
        ImGui::NextColumn();
        jumpOut = RenderSymbolAsmView( iptotalAsm, ipcountAsm, worker );
        ImGui::EndColumns();
        break;
    default:
        assert( false );
        break;
    }

    if( jumpOut != 0 )
    {
        auto sym = worker.GetSymbolData( jumpOut );
        if( sym )
        {
            auto line = sym->line;
            auto file = line == 0 ? nullptr : worker.GetString( sym->file );
            if( file && !SourceFileValid( file, worker.GetCaptureTime() ) )
            {
                file = nullptr;
                line = 0;
            }
            if( line > 0 || sym->size.Val() > 0 )
            {
                OpenSymbol( file, line, jumpOut, jumpOut, worker );
            }
        }
    }
}

static uint32_t GetHotnessColor( uint32_t ipSum, uint32_t maxIpCount )
{
    const auto ipPercent = float( ipSum ) / maxIpCount;
    if( ipPercent <= 0.5f )
    {
        const auto a = int( ( ipPercent * 1.5f + 0.25f ) * 255 );
        return 0x000000FF | ( a << 24 );
    }
    else if( ipPercent <= 1.f )
    {
        const auto g = int( ( ipPercent - 0.5f ) * 511 );
        return 0xFF0000FF | ( g << 8 );
    }
    else if( ipPercent <= 2.f )
    {
        const auto b = int( ( ipPercent - 1.f ) * 255 );
        return 0xFF00FFFF | ( b << 16 );
    }
    else
    {
        return 0xFFFFFFFF;
    }

}

void SourceView::RenderSymbolSourceView( uint32_t iptotal, unordered_flat_map<uint64_t, uint32_t> ipcount, const Worker& worker )
{
    if( m_sourceFiles.empty() )
    {
        TextColoredUnformatted( ImVec4( 1.f, 1.f, 0.2f, 1.f ), ICON_FA_EXCLAMATION_TRIANGLE );
        ImGui::SameLine();
        TextColoredUnformatted( ImVec4( 1.f, 0.3f, 0.3f, 1.f ), "The source file contents might not reflect the actual profiled code!" );
        ImGui::SameLine();
        TextColoredUnformatted( ImVec4( 1.f, 1.f, 0.2f, 1.f ), ICON_FA_EXCLAMATION_TRIANGLE );
    }
    else
    {
        TextColoredUnformatted( ImVec4( 1.f, 1.f, 0.2f, 1.f ), ICON_FA_EXCLAMATION_TRIANGLE );
        if( ImGui::IsItemHovered() )
        {
            ImGui::BeginTooltip();
            TextColoredUnformatted( ImVec4( 1.f, 1.f, 0.2f, 1.f ), ICON_FA_EXCLAMATION_TRIANGLE );
            ImGui::SameLine();
            TextColoredUnformatted( ImVec4( 1.f, 0.3f, 0.3f, 1.f ), "The source file contents might not reflect the actual profiled code!" );
            ImGui::SameLine();
            TextColoredUnformatted( ImVec4( 1.f, 1.f, 0.2f, 1.f ), ICON_FA_EXCLAMATION_TRIANGLE );
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
        TextDisabledUnformatted( ICON_FA_FILE " File:" );
        ImGui::SameLine();
        const auto fileColor = GetHsvColor( m_fileStringIdx, 0 );
        SmallColorBox( fileColor );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( -1 );
        ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
        if( ImGui::BeginCombo( "##fileList", m_file, ImGuiComboFlags_HeightLarge ) )
        {
            for( auto& v : m_sourceFiles )
            {
                const auto color = GetHsvColor( v.first, 0 );
                SmallColorBox( color );
                ImGui::SameLine();
                auto fstr = worker.GetString( StringIdx( v.first ) );
                if( SourceFileValid( fstr, worker.GetCaptureTime() ) )
                {
                    ImGui::PushID( v.first );
                    if( ImGui::Selectable( fstr, fstr == m_file ) )
                    {
                        ParseSource( fstr, &worker );
                        m_targetLine = v.second;
                        SelectLine( v.second, &worker );
                    }
                }
                else
                {
                    TextDisabledUnformatted( fstr );
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleVar();
    }

    ImGui::BeginChild( "##sourceView", ImVec2( 0, 0 ), true, ImGuiWindowFlags_NoMove );
    if( m_font ) ImGui::PushFont( m_font );

    m_selectedAddressesHover.clear();
    if( m_targetLine != 0 )
    {
        int lineNum = 1;
        for( auto& line : m_lines )
        {
            if( m_targetLine == lineNum )
            {
                m_targetLine = 0;
                ImGui::SetScrollHereY();
            }
            RenderLine( line, lineNum++, 0, iptotal, &worker );
        }
    }
    else
    {
        ImGuiListClipper clipper( (int)m_lines.size() );
        while( clipper.Step() )
        {
            if( iptotal == 0 )
            {
                for( auto i=clipper.DisplayStart; i<clipper.DisplayEnd; i++ )
                {
                    RenderLine( m_lines[i], i+1, 0, 0, &worker );
                }
            }
            else
            {
                for( auto i=clipper.DisplayStart; i<clipper.DisplayEnd; i++ )
                {
                    auto it = ipcount.find( i+1 );
                    const auto ipcnt = it == ipcount.end() ? 0 : it->second;
                    RenderLine( m_lines[i], i+1, ipcnt, iptotal, &worker );
                }
            }
        }
    }

    auto win = ImGui::GetCurrentWindow();
    if( win->ScrollbarY )
    {
        auto draw = ImGui::GetWindowDrawList();
        auto rect = ImGui::GetWindowScrollbarRect( win, ImGuiAxis_Y );
        ImGui::PushClipRect( rect.Min, rect.Max, false );
        if( m_selectedLine != 0 )
        {
            const auto ly = round( rect.Min.y + ( m_selectedLine - 0.5f ) / m_lines.size() * rect.GetHeight() );
            draw->AddLine( ImVec2( rect.Min.x, ly ), ImVec2( rect.Max.x, ly ), 0x8899994C, 3 );
        }
        if( m_fileStringIdx == m_hoveredSource && m_hoveredLine != 0 )
        {
            const auto ly = round( rect.Min.y + ( m_hoveredLine - 0.5f ) / m_lines.size() * rect.GetHeight() );
            draw->AddLine( ImVec2( rect.Min.x, ly ), ImVec2( rect.Max.x, ly ), 0x88888888, 3 );
        }

        uint32_t maxIpCount = 0;
        std::vector<std::pair<uint64_t, uint32_t>> ipData;
        ipData.reserve( ipcount.size() );
        for( auto& v : ipcount )
        {
            if( v.second > maxIpCount ) maxIpCount = v.second;
            ipData.emplace_back( v.first, v.second );
        }
        pdqsort_branchless( ipData.begin(), ipData.end(), []( const auto& l, const auto& r ) { return l.first < r.first; } );

        const auto step = uint32_t( m_lines.size() * 2 / rect.GetHeight() );
        const auto x14 = round( rect.Min.x + rect.GetWidth() * 0.4f );
        const auto x34 = round( rect.Min.x + rect.GetWidth() * 0.6f );

        auto it = ipData.begin();
        while( it != ipData.end() )
        {
            const auto firstLine = it->first;
            uint32_t ipSum = 0;
            while( it != ipData.end() && it->first <= firstLine + step )
            {
                ipSum += it->second;
                ++it;
            }
            const auto ly = round( rect.Min.y + float( firstLine ) / m_lines.size() * rect.GetHeight() );
            const uint32_t color = GetHotnessColor( ipSum, maxIpCount );
            draw->AddRectFilled( ImVec2( x14, ly ), ImVec2( x34, ly+3 ), color );
        }

        ImGui::PopClipRect();
    }

    if( m_font ) ImGui::PopFont();
    ImGui::EndChild();
}

uint64_t SourceView::RenderSymbolAsmView( uint32_t iptotal, unordered_flat_map<uint64_t, uint32_t> ipcount, const Worker& worker )
{
    SmallCheckbox( ICON_FA_SEARCH_LOCATION " Relative locations", &m_asmRelative );
    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();
    SmallCheckbox( ICON_FA_FILE_IMPORT " Show source locations", &m_asmShowSourceLocation );
    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();
    SmallCheckbox( ICON_FA_SHARE " Draw jumps", &m_showJumps );

    ImGui::BeginChild( "##asmView", ImVec2( 0, 0 ), true, ImGuiWindowFlags_NoMove );
    if( m_font ) ImGui::PushFont( m_font );

    int maxAddrLen;
    {
        char tmp[32];
        sprintf( tmp, "%" PRIx64, m_baseAddr + m_codeLen );
        maxAddrLen = strlen( tmp );
    }

    uint64_t selJumpStart = 0;
    uint64_t selJumpEnd;
    uint64_t selJumpTarget;
    uint64_t jumpOut = 0;

    if( m_targetAddr != 0 )
    {
        for( auto& line : m_asm )
        {
            if( m_targetAddr == line.addr )
            {
                m_targetAddr = 0;
                ImGui::SetScrollHereY();
            }
            RenderAsmLine( line, 0, iptotal, worker, jumpOut, maxAddrLen );
        }
    }
    else
    {
        const auto th = (int)ImGui::GetTextLineHeightWithSpacing();
        ImGuiListClipper clipper( (int)m_asm.size(), th );
        while( clipper.Step() )
        {
            assert( clipper.StepNo == 3 );
            const auto wpos = ImGui::GetCursorScreenPos();
            static std::vector<uint64_t> insList;
            insList.clear();
            if( iptotal == 0 )
            {
                for( auto i=clipper.DisplayStart; i<clipper.DisplayEnd; i++ )
                {
                    RenderAsmLine( m_asm[i], 0, 0, worker, jumpOut, maxAddrLen );
                    insList.emplace_back( m_asm[i].addr );
                }
            }
            else
            {
                for( auto i=clipper.DisplayStart; i<clipper.DisplayEnd; i++ )
                {
                    auto& line = m_asm[i];
                    auto it = ipcount.find( line.addr );
                    const auto ipcnt = it == ipcount.end() ? 0 : it->second;
                    RenderAsmLine( line, ipcnt, iptotal, worker, jumpOut, maxAddrLen );
                    insList.emplace_back( line.addr );
                }
            }
            if( m_showJumps && !m_jumpTable.empty() )
            {
                auto draw = ImGui::GetWindowDrawList();
                const auto ts = ImGui::CalcTextSize( " " );
                const auto th2 = floor( ts.y / 2 );
                const auto th4 = floor( ts.y / 4 );
                const auto xoff = ( iptotal == 0 ? 0 : ( 7 * ts.x + ts.y ) ) + (3+maxAddrLen) * ts.x + ( m_asmShowSourceLocation ? 36 * ts.x : 0 );
                const auto minAddr = m_asm[clipper.DisplayStart].addr;
                const auto maxAddr = m_asm[clipper.DisplayEnd-1].addr;
                const auto mjl = m_maxJumpLevel;
                const auto JumpArrow = JumpArrowBase * ts.y / 15;

                int i = -1;
                for( auto& v : m_jumpTable )
                {
                    i++;
                    if( v.second.min > maxAddr || v.second.max < minAddr ) continue;
                    const auto col = GetHsvColor( i, 0 );

                    auto it0 = std::lower_bound( insList.begin(), insList.end(), v.second.min );
                    auto it1 = std::lower_bound( insList.begin(), insList.end(), v.second.max );
                    const auto y0 = ( it0 == insList.end() || *it0 != v.second.min ) ? -th : ( it0 - insList.begin() ) * th;
                    const auto y1 = it1 == insList.end() ? ( insList.size() + 1 ) * th  : ( it1 - insList.begin() ) * th;

                    float thickness = 1;
                    if( ImGui::IsMouseHoveringRect( wpos + ImVec2( xoff + JumpSeparation * ( mjl - v.second.level ) - JumpSeparation / 2, y0 + th2 ), wpos + ImVec2( xoff + JumpSeparation * ( mjl - v.second.level ) + JumpSeparation / 2, y1 + th2 ) ) )
                    {
                        thickness = 2;
                        if( m_font ) ImGui::PopFont();
                        ImGui::BeginTooltip();
                        char tmp[32];
                        sprintf( tmp, "+%" PRIu64, v.first - m_baseAddr );
                        TextFocused( "Jump target:", tmp );
                        ImGui::SameLine();
                        sprintf( tmp, "(0x%" PRIx64 ")", v.first );
                        TextDisabledUnformatted( tmp );
                        uint32_t srcline;
                        const auto srcidx = worker.GetLocationForAddress( v.first, srcline );
                        if( srcline != 0 )
                        {
                            const auto fileName = worker.GetString( srcidx );
                            const auto fileColor = GetHsvColor( srcidx.Idx(), 0 );
                            TextDisabledUnformatted( "Source location:" );
                            ImGui::SameLine();
                            SmallColorBox( fileColor );
                            ImGui::SameLine();
                            ImGui::Text( "%s:%i", fileName, srcline );
                        }
                        TextFocused( "Jump range:", MemSizeToString( v.second.max - v.second.min ) );
                        TextFocused( "Jump sources:", RealToString( v.second.source.size() ) );
                        ImGui::EndTooltip();
                        if( m_font ) ImGui::PushFont( m_font );
                        if( ImGui::IsMouseClicked( 0 ) )
                        {
                            m_targetAddr = v.first;
                            m_selectedAddresses.clear();
                            m_selectedAddresses.emplace( v.first );
                        }
                        selJumpStart = v.second.min;
                        selJumpEnd = v.second.max;
                        selJumpTarget = v.first;
                    }

                    draw->AddLine( wpos + ImVec2( xoff + JumpSeparation * ( mjl - v.second.level ), y0 + th2 ), wpos + ImVec2( xoff + JumpSeparation * ( mjl - v.second.level ), y1 + th2 ), col, thickness );

                    if( v.first >= minAddr && v.first <= maxAddr )
                    {
                        auto iit = std::lower_bound( insList.begin(), insList.end(), v.first );
                        assert( iit != insList.end() );
                        const auto y = ( iit - insList.begin() ) * th;
                        draw->AddLine( wpos + ImVec2( xoff + JumpSeparation * ( mjl - v.second.level ), y + th2 ), wpos + ImVec2( xoff + JumpSeparation * mjl + JumpArrow + 1, y + th2 ), col, thickness );
                        draw->AddLine( wpos + ImVec2( xoff + JumpSeparation * mjl + JumpArrow, y + th2 ), wpos + ImVec2( xoff + JumpSeparation * mjl + JumpArrow - th4, y + th2 - th4 ), col, thickness );
                        draw->AddLine( wpos + ImVec2( xoff + JumpSeparation * mjl + JumpArrow, y + th2 ), wpos + ImVec2( xoff + JumpSeparation * mjl + JumpArrow - th4, y + th2 + th4 ), col, thickness );
                    }
                    for( auto& s : v.second.source )
                    {
                        if( s >= minAddr && s <= maxAddr )
                        {
                            auto iit = std::lower_bound( insList.begin(), insList.end(), s );
                            assert( iit != insList.end() );
                            const auto y = ( iit - insList.begin() ) * th;
                            draw->AddLine( wpos + ImVec2( xoff + JumpSeparation * ( mjl - v.second.level ), y + th2 ), wpos + ImVec2( xoff + JumpSeparation * mjl + JumpArrow, y + th2 ), col, thickness );
                        }
                    }
                }
            }
        }
    }

    auto win = ImGui::GetCurrentWindow();
    if( win->ScrollbarY )
    {
        auto draw = ImGui::GetWindowDrawList();
        auto rect = ImGui::GetWindowScrollbarRect( win, ImGuiAxis_Y );
        ImGui::PushClipRect( rect.Min, rect.Max, false );
        std::vector<uint32_t> lineOff;
        lineOff.reserve( std::max( m_selectedAddresses.size(), m_selectedAddressesHover.size() ) );
        if( !m_selectedAddresses.empty() )
        {
            for( size_t i=0; i<m_asm.size(); i++ )
            {
                if( m_selectedAddresses.find( m_asm[i].addr ) != m_selectedAddresses.end() )
                {
                    lineOff.push_back( uint32_t( i ) );
                }
            }
            float lastLine = 0;
            for( auto& v : lineOff )
            {
                const auto ly = round( rect.Min.y + ( v - 0.5f ) / m_asm.size() * rect.GetHeight() );
                if( ly > lastLine )
                {
                    lastLine = ly;
                    draw->AddLine( ImVec2( rect.Min.x, ly ), ImVec2( rect.Max.x, ly ), 0x8899994C, 1 );
                }
            }
        }
        if( !m_selectedAddressesHover.empty() )
        {
            lineOff.clear();
            for( size_t i=0; i<m_asm.size(); i++ )
            {
                if( m_selectedAddressesHover.find( m_asm[i].addr ) != m_selectedAddressesHover.end() )
                {
                    lineOff.push_back( uint32_t( i ) );
                }
            }
            float lastLine = 0;
            for( auto& v : lineOff )
            {
                const auto ly = round( rect.Min.y + ( v - 0.5f ) / m_asm.size() * rect.GetHeight() );
                if( ly > lastLine )
                {
                    lastLine = ly;
                    draw->AddLine( ImVec2( rect.Min.x, ly ), ImVec2( rect.Max.x, ly ), 0x88888888, 1 );
                }
            }
        }

        uint32_t selJumpLineStart, selJumpLineEnd, selJumpLineTarget;
        uint32_t maxIpCount = 0;
        std::vector<std::pair<uint64_t, uint32_t>> ipData;
        ipData.reserve( ipcount.size() );
        if( selJumpStart == 0 )
        {
            for( size_t i=0; i<m_asm.size(); i++ )
            {
                auto it = ipcount.find( m_asm[i].addr );
                if( it == ipcount.end() ) continue;
                if( it->second > maxIpCount ) maxIpCount = it->second;
                ipData.emplace_back( i, it->second );
            }
        }
        else
        {
            for( size_t i=0; i<m_asm.size(); i++ )
            {
                if( selJumpStart == m_asm[i].addr ) selJumpLineStart = i;
                if( selJumpEnd == m_asm[i].addr ) selJumpLineEnd = i;
                if( selJumpTarget == m_asm[i].addr ) selJumpLineTarget = i;

                auto it = ipcount.find( m_asm[i].addr );
                if( it == ipcount.end() ) continue;
                if( it->second > maxIpCount ) maxIpCount = it->second;
                ipData.emplace_back( i, it->second );
            }
        }
        pdqsort_branchless( ipData.begin(), ipData.end(), []( const auto& l, const auto& r ) { return l.first < r.first; } );

        const auto step = uint32_t( m_asm.size() * 2 / rect.GetHeight() );
        const auto x40 = round( rect.Min.x + rect.GetWidth() * 0.4f );
        const auto x60 = round( rect.Min.x + rect.GetWidth() * 0.6f );

        auto it = ipData.begin();
        while( it != ipData.end() )
        {
            const auto firstLine = it->first;
            uint32_t ipSum = 0;
            while( it != ipData.end() && it->first <= firstLine + step )
            {
                ipSum += it->second;
                ++it;
            }
            const auto ly = round( rect.Min.y + float( firstLine ) / m_asm.size() * rect.GetHeight() );
            const uint32_t color = GetHotnessColor( ipSum, maxIpCount );
            draw->AddRectFilled( ImVec2( x40, ly ), ImVec2( x60, ly+3 ), color );
        }

        if( selJumpStart != 0 )
        {
            const auto yStart = rect.Min.y + float( selJumpLineStart ) / m_asm.size() * rect.GetHeight();
            const auto yEnd = rect.Min.y + float( selJumpLineEnd ) / m_asm.size() * rect.GetHeight();
            const auto yTarget = rect.Min.y + float( selJumpLineTarget ) / m_asm.size() * rect.GetHeight();
            const auto x50 = round( rect.Min.x + rect.GetWidth() * 0.5f ) - 1;
            const auto x25 = round( rect.Min.x + rect.GetWidth() * 0.25f );
            const auto x75 = round( rect.Min.x + rect.GetWidth() * 0.75f );
            draw->AddLine( ImVec2( x50, yStart ), ImVec2( x50, yEnd ), 0xFF00FF00 );
            draw->AddLine( ImVec2( x25, yTarget ), ImVec2( x75, yTarget ), 0xFF00FF00 );
        }
    }

    if( m_font ) ImGui::PopFont();
    ImGui::EndChild();

    return jumpOut;
}

static bool PrintPercentage( float val )
{
    const auto ty = ImGui::GetFontSize();
    auto draw = ImGui::GetWindowDrawList();
    const auto wpos = ImGui::GetCursorScreenPos();
    const auto stw = ImGui::CalcTextSize( " " ).x;
    const auto htw = stw / 2;
    const auto tw = stw * 8;

    char tmp[16];
    auto end = PrintFloat( tmp, tmp+16, val, 2 );
    memcpy( end, "%", 2 );
    end++;
    const auto sz = end - tmp;
    char buf[16];
    memset( buf, ' ', 7-sz );
    memcpy( buf + 7 - sz, tmp, sz+1 );

    draw->AddRectFilled( wpos, wpos + ImVec2( val * tw / 100, ty+1 ), 0xFF444444 );
    DrawTextContrast( draw, wpos + ImVec2( htw, 0 ), 0xFFFFFFFF, buf );

    ImGui::ItemSize( ImVec2( stw * 7, ty ), 0 );
    return ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect( wpos, wpos + ImVec2( stw * 7, ty ) );
}

void SourceView::RenderLine( const Line& line, int lineNum, uint32_t ipcnt, uint32_t iptotal, const Worker* worker )
{
    const auto ty = ImGui::GetFontSize();
    auto draw = ImGui::GetWindowDrawList();
    const auto w = ImGui::GetWindowWidth();
    const auto wpos = ImGui::GetCursorScreenPos();
    if( m_fileStringIdx == m_hoveredSource && lineNum == m_hoveredLine )
    {
        draw->AddRectFilled( wpos, wpos + ImVec2( w, ty+1 ), 0x22FFFFFF );
    }
    else if( lineNum == m_selectedLine )
    {
        draw->AddRectFilled( wpos, wpos + ImVec2( w, ty+1 ), 0xFF333322 );
    }

    if( iptotal != 0 )
    {
        if( ipcnt == 0 )
        {
            const auto ts = ImGui::CalcTextSize( " " );
            ImGui::ItemSize( ImVec2( 7 * ts.x, ts.y ) );
        }
        else
        {
            if( PrintPercentage( 100.f * ipcnt / iptotal ) )
            {
                if( m_font ) ImGui::PopFont();
                ImGui::BeginTooltip();
                if( worker ) TextFocused( "Time:", TimeToString( ipcnt * worker->GetSamplingPeriod() ) );
                TextFocused( "Sample count:", RealToString( ipcnt ) );
                ImGui::EndTooltip();
                if( m_font ) ImGui::PushFont( m_font );
            }
        }
        ImGui::SameLine( 0, ty );
    }

    const auto lineCount = m_lines.size();
    const auto tmp = RealToString( lineCount );
    const auto maxLine = strlen( tmp );
    const auto lineString = RealToString( lineNum );
    const auto linesz = strlen( lineString );
    char buf[16];
    memset( buf, ' ', maxLine - linesz );
    memcpy( buf + maxLine - linesz, lineString, linesz+1 );
    TextDisabledUnformatted( buf );
    ImGui::SameLine( 0, ty );

    uint32_t match = 0;
    if( m_symAddr != 0 )
    {
        assert( worker );
        const auto stw = ImGui::CalcTextSize( " " ).x;
        auto addresses = worker->GetAddressesForLocation( m_fileStringIdx, lineNum );
        if( addresses )
        {
            for( auto& addr : *addresses )
            {
                match += ( addr >= m_baseAddr && addr < m_baseAddr + m_codeLen );
            }
        }
        const auto tmp = RealToString( m_asm.size() );
        const auto maxAsm = strlen( tmp ) + 1;
        if( match > 0 )
        {
            const auto asmString = RealToString( match );
            sprintf( buf, "@%s", asmString );
            const auto asmsz = strlen( buf );
            TextDisabledUnformatted( buf );
            ImGui::SameLine( 0, 0 );
            ImGui::ItemSize( ImVec2( stw * ( maxAsm - asmsz ), ty ), 0 );
        }
        else
        {
            ImGui::ItemSize( ImVec2( stw * maxAsm, ty ), 0 );
        }
    }

    ImGui::SameLine( 0, ty );
    ImGui::TextUnformatted( line.begin, line.end );

    if( match > 0 && ImGui::IsMouseHoveringRect( wpos, wpos + ImVec2( w, ty+1 ) ) )
    {
        draw->AddRectFilled( wpos, wpos + ImVec2( w, ty+1 ), 0x11FFFFFF );
        if( ImGui::IsMouseClicked( 0 ) || ImGui::IsMouseClicked( 1 ) )
        {
            m_displayMode = DisplayMixed;
            SelectLine( lineNum, worker, ImGui::IsMouseClicked( 1 ) );
        }
        else
        {
            SelectAsmLinesHover( m_fileStringIdx, lineNum, *worker );
        }
    }

    draw->AddLine( wpos + ImVec2( 0, ty+2 ), wpos + ImVec2( w, ty+2 ), 0x08FFFFFF );
}

void SourceView::RenderAsmLine( const AsmLine& line, uint32_t ipcnt, uint32_t iptotal, const Worker& worker, uint64_t& jumpOut, int maxAddrLen )
{
    const auto ty = ImGui::GetFontSize();
    auto draw = ImGui::GetWindowDrawList();
    const auto w = ImGui::GetWindowWidth();
    const auto wpos = ImGui::GetCursorScreenPos();
    if( m_selectedAddressesHover.find( line.addr ) != m_selectedAddressesHover.end() )
    {
        draw->AddRectFilled( wpos, wpos + ImVec2( w, ty+1 ), 0x22FFFFFF );
    }
    else if( m_selectedAddresses.find( line.addr ) != m_selectedAddresses.end() )
    {
        draw->AddRectFilled( wpos, wpos + ImVec2( w, ty+1 ), 0xFF333322 );
    }
    if( line.addr == m_highlightAddr )
    {
        draw->AddRectFilled( wpos, wpos + ImVec2( w, ty+1 ), 0xFF222233 );
    }

    if( iptotal != 0 )
    {
        if( ipcnt == 0 )
        {
            const auto ts = ImGui::CalcTextSize( " " );
            ImGui::ItemSize( ImVec2( 7 * ts.x, ts.y ) );
        }
        else
        {
            if( PrintPercentage( 100.f * ipcnt / iptotal ) )
            {
                if( m_font ) ImGui::PopFont();
                ImGui::BeginTooltip();
                TextFocused( "Time:", TimeToString( ipcnt * worker.GetSamplingPeriod() ) );
                TextFocused( "Sample count:", RealToString( ipcnt ) );
                ImGui::EndTooltip();
                if( m_font ) ImGui::PushFont( m_font );
            }

        }
        ImGui::SameLine( 0, ty );
    }

    char buf[256];
    if( m_asmRelative )
    {
        sprintf( buf, "+%" PRIu64, line.addr - m_baseAddr );
    }
    else
    {
        sprintf( buf, "%" PRIx64, line.addr );
    }
    const auto asz = strlen( buf );
    memset( buf+asz, ' ', maxAddrLen-asz );
    buf[maxAddrLen] = '\0';
    TextDisabledUnformatted( buf );

    bool lineHovered = false;
    if( m_asmShowSourceLocation )
    {
        const auto stw = ImGui::CalcTextSize( " " ).x;
        ImGui::SameLine();
        uint32_t srcline;
        const auto srcidx = worker.GetLocationForAddress( line.addr, srcline );
        if( srcline != 0 )
        {
            const auto fileName = worker.GetString( srcidx );
            const auto fileColor = GetHsvColor( srcidx.Idx(), 0 );
            SmallColorBox( fileColor );
            ImGui::SameLine();
            const auto lineString = RealToString( srcline );
            const auto linesz = strlen( lineString );
            char buf[32];
            const auto fnsz = strlen( fileName );
            if( fnsz < 30 - m_maxLine )
            {
                sprintf( buf, "%s:%i", fileName, srcline );
            }
            else
            {
                sprintf( buf, "...%s:%i", fileName+fnsz-(30-3-1-m_maxLine), srcline );
            }
            const auto bufsz = strlen( buf );
            TextDisabledUnformatted( buf );
            if( ImGui::IsItemHovered() )
            {
                lineHovered = true;
                if( m_font ) ImGui::PopFont();
                ImGui::BeginTooltip();
                ImGui::Text( "%s:%i", fileName, srcline );
                ImGui::EndTooltip();
                if( m_font ) ImGui::PushFont( m_font );
                if( ImGui::IsItemClicked( 0 ) || ImGui::IsItemClicked( 1 ) )
                {
                    if( m_file == fileName )
                    {
                        if( ImGui::IsMouseClicked( 1 ) ) m_targetLine = srcline;
                        SelectLine( srcline, &worker, false );
                        m_displayMode = DisplayMixed;
                    }
                    else if( SourceFileValid( fileName, worker.GetCaptureTime() ) )
                    {
                        ParseSource( fileName, &worker );
                        m_targetLine = srcline;
                        SelectLine( srcline, &worker, false );
                        m_displayMode = DisplayMixed;
                    }
                    else
                    {
                        SelectAsmLines( srcidx.Idx(), srcline, worker, false );
                    }
                }
                else
                {
                    m_hoveredLine = srcline;
                    m_hoveredSource = srcidx.Idx();
                }
            }
            ImGui::SameLine( 0, 0 );
            ImGui::ItemSize( ImVec2( stw * ( 32 - bufsz ), ty ), 0 );
        }
        else
        {
            SmallColorBox( 0 );
            ImGui::SameLine( 0, 0 );
            ImGui::ItemSize( ImVec2( stw * 32, ty ), 0 );
        }
    }
    if( m_showJumps )
    {
        const auto JumpArrow = JumpArrowBase * ty / 15;;
        ImGui::SameLine( 0, 2*ty + JumpArrow + m_maxJumpLevel * JumpSeparation );
        auto jit = m_jumpOut.find( line.addr );
        if( jit != m_jumpOut.end() )
        {
            const auto ts = ImGui::CalcTextSize( " " );
            const auto th2 = floor( ts.y / 2 );
            const auto th4 = floor( ts.y / 4 );
            const auto& mjl = m_maxJumpLevel;
            const auto col = GetHsvColor( line.jumpAddr, 6 );
            const auto xoff = ( iptotal == 0 ? 0 : ( 7 * ts.x + ts.y ) ) + (3+maxAddrLen) * ts.x + ( m_asmShowSourceLocation ? 36 * ts.x : 0 );

            draw->AddLine( wpos + ImVec2( xoff + JumpSeparation * mjl + th2, th2 ), wpos + ImVec2( xoff + JumpSeparation * mjl + th2 + JumpArrow / 2, th2 ), col );
            draw->AddLine( wpos + ImVec2( xoff + JumpSeparation * mjl + th2, th2 ), wpos + ImVec2( xoff + JumpSeparation * mjl + th2 + th4, th2 - th4 ), col );
            draw->AddLine( wpos + ImVec2( xoff + JumpSeparation * mjl + th2, th2 ), wpos + ImVec2( xoff + JumpSeparation * mjl + th2 + th4, th2 + th4 ), col );
        }
    }
    else
    {
        ImGui::SameLine( 0, ty );
    }

    const auto msz = line.mnemonic.size();
    memcpy( buf, line.mnemonic.c_str(), msz );
    memset( buf+msz, ' ', m_maxMnemonicLen-msz );
    memcpy( buf+m_maxMnemonicLen, line.operands.c_str(), line.operands.size() + 1 );
    ImGui::TextUnformatted( buf );

    if( line.jumpAddr != 0 )
    {
        uint32_t offset = 0;
        const auto base = worker.GetSymbolForAddress( line.jumpAddr, offset );
        auto sym = base == 0 ? worker.GetSymbolData( line.jumpAddr ) : worker.GetSymbolData( base );
        if( sym )
        {
            ImGui::SameLine();
            ImGui::Spacing();
            ImGui::SameLine();
            if( base == m_baseAddr )
            {
                ImGui::TextDisabled( "-> [%s+%" PRIu32"]", worker.GetString( sym->name ), offset );
                if( ImGui::IsItemHovered() )
                {
                    m_highlightAddr = line.jumpAddr;
                    if( ImGui::IsItemClicked() )
                    {
                        m_targetAddr = line.jumpAddr;
                        m_selectedAddresses.clear();
                        m_selectedAddresses.emplace( line.jumpAddr );
                    }
                }
            }
            else
            {
                ImGui::TextDisabled( "[%s+%" PRIu32"]", worker.GetString( sym->name ), offset );
                if( ImGui::IsItemClicked() ) jumpOut = line.jumpAddr;
            }
        }
    }

    if( lineHovered )
    {
        draw->AddRectFilled( wpos, wpos + ImVec2( w, ty+1 ), 0x11FFFFFF );
    }

    draw->AddLine( wpos + ImVec2( 0, ty+2 ), wpos + ImVec2( w, ty+2 ), 0x08FFFFFF );
}

void SourceView::SelectLine( uint32_t line, const Worker* worker, bool changeAsmLine, uint64_t targetAddr )
{
    m_selectedLine = line;
    if( m_symAddr == 0 ) return;
    assert( worker );
    SelectAsmLines( m_fileStringIdx, line, *worker, changeAsmLine, targetAddr );
}

void SourceView::SelectAsmLines( uint32_t file, uint32_t line, const Worker& worker, bool changeAsmLine, uint64_t targetAddr )
{
    m_selectedAddresses.clear();
    auto addresses = worker.GetAddressesForLocation( file, line );
    if( addresses )
    {
        const auto& addr = *addresses;
        if( changeAsmLine )
        {
            if( targetAddr != 0 )
            {
                m_targetAddr == targetAddr;
            }
            else
            {
                for( auto& v : addr )
                {
                    if( v >= m_baseAddr && v < m_baseAddr + m_codeLen )
                    {
                        m_targetAddr = v;
                        break;
                    }
                }
            }
        }
        for( auto& v : addr )
        {
            if( v >= m_baseAddr && v < m_baseAddr + m_codeLen )
            {
                m_selectedAddresses.emplace( v );
            }
        }
    }
}

void SourceView::SelectAsmLinesHover( uint32_t file, uint32_t line, const Worker& worker )
{
    assert( m_selectedAddressesHover.empty() );
    auto addresses = worker.GetAddressesForLocation( file, line );
    if( addresses )
    {
        for( auto& v : *addresses )
        {
            if( v >= m_baseAddr && v < m_baseAddr + m_codeLen )
            {
                m_selectedAddressesHover.emplace( v );
            }
        }
    }
}

void SourceView::GatherIpStats( uint64_t addr, uint32_t& iptotalSrc, uint32_t& iptotalAsm, unordered_flat_map<uint64_t, uint32_t>& ipcountSrc, unordered_flat_map<uint64_t, uint32_t>& ipcountAsm, const Worker& worker )
{
    auto ipmap = worker.GetSymbolInstructionPointers( addr );
    if( !ipmap ) return;
    for( auto& ip : *ipmap )
    {
        if( m_file )
        {
            auto frame = worker.GetCallstackFrame( ip.first );
            if( frame )
            {
                auto ffn = worker.GetString( frame->data[0].file );
                if( strcmp( ffn, m_file ) == 0 )
                {
                    const auto line = frame->data[0].line;
                    auto it = ipcountSrc.find( line );
                    if( it == ipcountSrc.end() )
                    {
                        ipcountSrc.emplace( line, ip.second );
                    }
                    else
                    {
                        it->second += ip.second;
                    }
                    iptotalSrc += ip.second;
                }
            }
        }

        auto addr = worker.GetCanonicalPointer( ip.first );
        assert( ipcountAsm.find( addr ) == ipcountAsm.end() );
        ipcountAsm.emplace( addr, ip.second );
        iptotalAsm += ip.second;
    }
}

}
