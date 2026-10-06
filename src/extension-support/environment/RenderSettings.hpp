// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>

namespace wxl::render::settings
{
    struct Field { const char* name; float minimum,maximum; bool boolean=false; };
    enum class Result { Ok,Missing,Invalid,IoError };
    inline const char* Message(Result result)
    {
        switch(result) {
        case Result::Ok: return "Done.";
        case Result::Missing: return "No saved settings yet; current values kept.";
        case Result::Invalid: return "Invalid settings; current values kept.";
        default: return "File operation failed; current values kept.";
        }
    }
    inline std::string_view Trim(std::string_view s)
    {
        while(!s.empty() && (s.front()==' ' || s.front()=='\t' || s.front()=='\r')) s.remove_prefix(1);
        while(!s.empty() && (s.back()==' ' || s.back()=='\t' || s.back()=='\r')) s.remove_suffix(1);
        return s;
    }
    template<size_t N> bool Valid(const Field (&fields)[N],const std::array<float,N>& values)
    {
        for(size_t i=0;i<N;++i)
            if(!std::isfinite(values[i]) || values[i]<fields[i].minimum || values[i]>fields[i].maximum ||
                (fields[i].boolean && values[i]!=0 && values[i]!=1)) return false;
        return true;
    }
    // Transactional parsing: complete known schema only, never partially applies a damaged file.
    template<size_t N> Result Parse(std::string_view text,const Field (&fields)[N],std::array<float,N>& output)
    {
        if(text.size()>16384) return Result::Invalid;
        std::array<float,N> candidate{}; std::array<bool,N> seen{}; bool version=false;
        while(!text.empty())
        {
            const size_t end=text.find('\n'); auto line=Trim(text.substr(0,end));
            if(end==text.npos) text={}; else text.remove_prefix(end+1);
            if(line.empty() || line.front()=='#') continue;
            const size_t equal=line.find('='); if(equal==line.npos) return Result::Invalid;
            const auto key=Trim(line.substr(0,equal)),value=Trim(line.substr(equal+1));
            if(key=="version") { if(version || value!="1") return Result::Invalid; version=true; continue; }
            size_t index=0; while(index<N && key!=fields[index].name) ++index;
            if(index==N || seen[index]) return Result::Invalid;
            float parsed=0; const auto read=std::from_chars(value.data(),value.data()+value.size(),parsed);
            if(read.ec!=std::errc{} || read.ptr!=value.data()+value.size()) return Result::Invalid;
            candidate[index]=parsed; seen[index]=true;
        }
        if(!version || !Valid(fields,candidate)) return Result::Invalid;
        for(bool found:seen) if(!found) return Result::Invalid;
        output=candidate; return Result::Ok;
    }
    template<size_t N> Result Load(const std::filesystem::path& path,const Field (&fields)[N],std::array<float,N>& output)
    {
        std::error_code error; const bool exists=std::filesystem::exists(path,error);
        if(error) return Result::IoError; if(!exists) return Result::Missing;
        const auto size=std::filesystem::file_size(path,error);
        if(error) return Result::IoError; if(size>16384) return Result::Invalid;
        std::ifstream file(path,std::ios::binary); if(!file) return Result::IoError;
        std::string text(static_cast<size_t>(size),'\0');
        if(size && !file.read(text.data(),static_cast<std::streamsize>(size))) return Result::IoError;
        // Reject a concurrent growth rather than reading a valid prefix of a changed file.
        if(file.peek()!=std::char_traits<char>::eof()) return Result::Invalid;
        return Parse(text,fields,output);
    }
    template<size_t N> Result Save(const std::filesystem::path& path,const Field (&fields)[N],const std::array<float,N>& values)
    {
        if(!Valid(fields,values)) return Result::Invalid;
        std::string text="# WarcraftXL render settings. Saved explicitly from the panel.\nversion=1\n";
        for(size_t i=0;i<N;++i)
        {
            char number[48]; const auto wrote=std::to_chars(number,number+sizeof(number),values[i],std::chars_format::general,std::numeric_limits<float>::max_digits10);
            if(wrote.ec!=std::errc{}) return Result::Invalid;
            text+=fields[i].name; text+='='; text.append(number,wrote.ptr); text+='\n';
        }
        auto temp=path; temp+=L".tmp-"+std::to_wstring(GetCurrentProcessId());
        HANDLE file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE) return Result::IoError;
        DWORD written=0;
        const bool complete=WriteFile(file,text.data(),static_cast<DWORD>(text.size()),&written,nullptr) && written==text.size() && FlushFileBuffers(file);
        const bool closed=CloseHandle(file)!=FALSE;
        if(!complete || !closed || !MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        { DeleteFileW(temp.c_str()); return Result::IoError; }
        return Result::Ok;
    }
}
