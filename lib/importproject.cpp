/*
 * Cppcheck - A tool for static C/C++ code analysis
 * Copyright (C) 2007-2017 Cppcheck team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "importproject.h"

#include "path.h"
#include "settings.h"
#include "tinyxml2.h"
#include "token.h"
#include "tokenize.h"
#include "tokenlist.h"
#include "utils.h"

#include <cstring>
#include <fstream>
#include <utility>


void ImportProject::ignorePaths(const std::vector<std::string> &ipaths)
{
    for (std::list<FileSettings>::iterator it = fileSettings.begin(); it != fileSettings.end();) {
        bool ignore = false;
        for (std::size_t i = 0; i < ipaths.size(); ++i) {
            if (it->filename.size() > ipaths[i].size() && it->filename.compare(0,ipaths[i].size(),ipaths[i])==0) {
                ignore = true;
                break;
            }
        }
        if (ignore)
            fileSettings.erase(it++);
        else
            ++it;
    }
}

void ImportProject::ignoreOtherConfigs(const std::string &cfg)
{
    for (std::list<FileSettings>::iterator it = fileSettings.begin(); it != fileSettings.end();) {
        if (it->cfg != cfg)
            fileSettings.erase(it++);
        else
            ++it;
    }
}

void ImportProject::ignoreOtherPlatforms(cppcheck::Platform::PlatformType platformType)
{
    for (std::list<FileSettings>::iterator it = fileSettings.begin(); it != fileSettings.end();) {
        if (it->platformType != cppcheck::Platform::Unspecified && it->platformType != platformType)
            fileSettings.erase(it++);
        else
            ++it;
    }
}

void ImportProject::FileSettings::setDefines(std::string defs)
{
    while (defs.find(";%(") != std::string::npos) {
        std::string::size_type pos1 = defs.find(";%(");
        std::string::size_type pos2 = defs.find(';', pos1+1);
        defs.erase(pos1, pos2 == std::string::npos ? pos2 : (pos2-pos1));
    }
    while (defs.find(";;") != std::string::npos)
        defs.erase(defs.find(";;"),1);
    if (!defs.empty() && endsWith(defs,';'))
        defs.erase(defs.size() - 1U); // TODO: Use std::string::pop_back() as soon as travis supports it
    bool eq = false;
    for (std::size_t pos = 0; pos < defs.size(); ++pos) {
        if (defs[pos] == '(' || defs[pos] == '=')
            eq = true;
        else if (defs[pos] == ';') {
            if (!eq) {
                defs.insert(pos,"=1");
                pos += 3;
            }
            if (pos < defs.size())
                eq = false;
        }
    }
    if (!eq && !defs.empty())
        defs += "=1";
    defines.swap(defs);
}

static bool simplifyPathWithVariables(std::string &s, std::map<std::string, std::string, cppcheck::stricmp> &variables)
{
    std::set<std::string, cppcheck::stricmp> expanded;
    std::string::size_type start = 0;
    while ((start = s.find("$(")) != std::string::npos) {
        std::string::size_type end = s.find(')',start);
        if (end == std::string::npos)
            break;
        const std::string var = s.substr(start+2,end-start-2);
        if (expanded.find(var) != expanded.end())
            break;
        expanded.insert(var);
        std::map<std::string, std::string, cppcheck::stricmp>::const_iterator it1 = variables.find(var);
        // variable was not found within defined variables
        if (it1 == variables.end()) {
            const char *envValue = std::getenv(var.c_str());
            if (!envValue) {
                //! @TODO generate a debug/info message about undefined variable
                break;
            }
            variables[var] = std::string(envValue);
            it1 = variables.find(var);
        }
        s = s.substr(0, start) + it1->second + s.substr(end + 1);
    }
    if (s.find("$(") != std::string::npos)
        return false;
    s = Path::simplifyPath(Path::fromNativeSeparators(s));
    return true;
}

void ImportProject::FileSettings::setIncludePaths(const std::string &basepath, const std::list<std::string> &in, std::map<std::string, std::string, cppcheck::stricmp> &variables)
{
    std::list<std::string> I;
    // only parse each includePath once - so remove duplicates
    std::list<std::string> uniqueIncludePaths = in;
    uniqueIncludePaths.sort();
    uniqueIncludePaths.unique();

    for (std::list<std::string>::const_iterator it = uniqueIncludePaths.begin(); it != uniqueIncludePaths.end(); ++it) {
        if (it->empty())
            continue;
        if (it->compare(0,2,"%(")==0)
            continue;
        std::string s(Path::fromNativeSeparators(*it));
        if (s[0] == '/' || (s.size() > 1U && s.compare(1,2,":/") == 0)) {
            if (!endsWith(s,'/'))
                s += '/';
            I.push_back(s);
            continue;
        }

        if (endsWith(s,'/')) // this is a temporary hack, simplifyPath can crash if path ends with '/'
            s.erase(s.size() - 1U); // TODO: Use std::string::pop_back() as soon as travis supports it

        if (s.find("$(")==std::string::npos) {
            s = Path::simplifyPath(basepath + s);
        } else {
            if (!simplifyPathWithVariables(s,variables))
                continue;
        }
        if (s.empty())
            continue;
        I.push_back(s + '/');
    }
    includePaths.swap(I);
}

void ImportProject::import(const std::string &filename)
{
    std::ifstream fin(filename);
    if (!fin.is_open())
        return;
    if (filename.find("compile_commands.json") != std::string::npos) {
        importCompileCommands(fin);
    } else if (filename.find(".sln") != std::string::npos) {
        std::string path(Path::getPathFromFilename(Path::fromNativeSeparators(filename)));
        if (!path.empty() && !endsWith(path,'/'))
            path += '/';
        importSln(fin,path);
    } else if (filename.find(".vcxproj") != std::string::npos) {
        std::map<std::string, std::string, cppcheck::stricmp> variables;
        importVcxproj(filename, variables, emptyString);
    } else if (filename.find(".bpr") != std::string::npos) {
        importBcb6Prj(filename);
    }
}

void ImportProject::importCompileCommands(std::istream &istr)
{
    std::map<std::string, std::string> values;

    // TODO: Use a JSON parser

    Settings settings;
    TokenList tokenList(&settings);
    tokenList.createTokens(istr);
    for (const Token *tok = tokenList.front(); tok; tok = tok->next()) {
        if (Token::Match(tok, "%str% : %str% [,}]")) {
            const std::string& key = tok->str();
            const std::string& value = tok->strAt(2);
            values[key.substr(1, key.size() - 2U)] = value.substr(1, value.size() - 2U);
        }

        else if (tok->str() == "}") {
            if (!values["file"].empty() && !values["command"].empty()) {
                struct FileSettings fs;
                fs.filename = Path::fromNativeSeparators(values["file"]);
                const std::string& command = values["command"];
                const std::string directory = Path::fromNativeSeparators(values["directory"]);
                std::string::size_type pos = 0;
                while (std::string::npos != (pos = command.find(' ',pos))) {
                    pos++;
                    if (pos >= command.size())
                        break;
                    if (command[pos] != '/' && command[pos] != '-')
                        continue;
                    pos++;
                    if (pos >= command.size())
                        break;
                    char F = command[pos++];
                    std::string fval;
                    while (pos < command.size() && command[pos] != ' ' && command[pos] != '=') {
                        if (command[pos] != '\\')
                            fval += command[pos];
                        pos++;
                    }
                    if (F=='D') {
                        std::string defval;
                        bool escape = false;
                        while (pos < command.size() && command[pos] != ' ') {
                            if (command[pos] != '\\') {
                                defval += command[pos];
                                escape = false;
                            } else {
                                if (escape) {
                                    defval += '\\';
                                    escape = false;
                                } else {
                                    escape = true;
                                }
                            }
                            pos++;
                        }
                        fs.defines += fval;
                        if (!defval.empty())
                            fs.defines += defval;
                        fs.defines += ';';
                    } else if (F=='U')
                        fs.undefs.insert(fval);
                    else if (F=='I')
                        fs.includePaths.push_back(fval);
                    else if (F=='s' && fval.compare(0,3,"td=") == 0)
                        fs.standard = fval.substr(3);
                    else if (F == 'i' && fval == "system") {
                        ++pos;
                        std::string isystem;
                        while (pos < command.size() && command[pos] != ' ') {
                            if (command[pos] != '\\')
                                isystem += command[pos];
                            pos++;
                        }
                        fs.systemIncludePaths.push_back(isystem);
                    }
                }
                std::map<std::string, std::string, cppcheck::stricmp> variables;
                fs.setIncludePaths(directory, fs.includePaths, variables);
                fs.setDefines(fs.defines);
                fileSettings.push_back(fs);
            }
            values.clear();
        }
    }
}

void ImportProject::importSln(std::istream &istr, const std::string &path)
{
    std::map<std::string,std::string,cppcheck::stricmp> variables;
    variables["SolutionDir"] = path;

    std::string line;
    while (std::getline(istr,line)) {
        if (line.compare(0,8,"Project(")!=0)
            continue;
        const std::string::size_type pos = line.find(".vcxproj");
        if (pos == std::string::npos)
            continue;
        const std::string::size_type pos1 = line.rfind('\"',pos);
        if (pos1 == std::string::npos)
            continue;
        std::string vcxproj(line.substr(pos1+1, pos-pos1+7));
        if (!Path::isAbsolute(vcxproj))
            vcxproj = path + vcxproj;
        importVcxproj(Path::fromNativeSeparators(vcxproj), variables, emptyString);
    }
}

namespace {
    struct ProjectConfiguration {
        explicit ProjectConfiguration(const tinyxml2::XMLElement *cfg) : platform(Unknown) {
            const char *a = cfg->Attribute("Include");
            if (a)
                name = a;
            for (const tinyxml2::XMLElement *e = cfg->FirstChildElement(); e; e = e->NextSiblingElement()) {
                if (!e->GetText())
                    continue;
                if (std::strcmp(e->Name(),"Configuration")==0)
                    configuration = e->GetText();
                else if (std::strcmp(e->Name(),"Platform")==0) {
                    platformStr = e->GetText();
                    if (platformStr == "Win32")
                        platform = Win32;
                    else if (platformStr == "x64")
                        platform = x64;
                    else
                        platform = Unknown;
                }
            }
        }
        std::string name;
        std::string configuration;
        enum { Win32, x64, Unknown } platform;
        std::string platformStr;
    };

    struct ItemDefinitionGroup {
        explicit ItemDefinitionGroup(const tinyxml2::XMLElement *idg, const std::string &includePaths) : additionalIncludePaths(includePaths) {
            const char *condAttr = idg->Attribute("Condition");
            if (condAttr)
                condition = condAttr;
            for (const tinyxml2::XMLElement *e1 = idg->FirstChildElement(); e1; e1 = e1->NextSiblingElement()) {
                if (std::strcmp(e1->Name(), "ClCompile") != 0)
                    continue;
                for (const tinyxml2::XMLElement *e = e1->FirstChildElement(); e; e = e->NextSiblingElement()) {
                    if (e->GetText()) {
                        if (std::strcmp(e->Name(), "PreprocessorDefinitions") == 0)
                            preprocessorDefinitions = e->GetText();
                        else if (std::strcmp(e->Name(), "AdditionalIncludeDirectories") == 0) {
                            if (!additionalIncludePaths.empty())
                                additionalIncludePaths += ';';
                            additionalIncludePaths += e->GetText();
                        }
                    }
                }
            }
        }

        static void replaceAll(std::string &c, const std::string &from, const std::string &to) {
            std::string::size_type pos;
            while ((pos = c.find(from)) != std::string::npos) {
                c.erase(pos,from.size());
                c.insert(pos,to);
            }
        }

        bool conditionIsTrue(const ProjectConfiguration &p) const {
            if (condition.empty())
                return true;
            std::string c = '(' + condition + ");";
            replaceAll(c, "$(Configuration)", p.configuration);
            replaceAll(c, "$(Platform)", p.platformStr);

            // TODO : Better evaluation
            Settings s;
            std::istringstream istr(c);
            Tokenizer tokenizer(&s, nullptr);
            tokenizer.tokenize(istr,"vcxproj");
            for (const Token *tok = tokenizer.tokens(); tok; tok = tok->next()) {
                if (tok->str() == "(" && tok->astOperand1() && tok->astOperand2()) {
                    if (tok->astOperand1()->expressionString() == "Configuration.Contains")
                        return ('\'' + p.configuration + '\'') == tok->astOperand2()->str();
                }
                if (tok->str() == "==" && tok->astOperand1() && tok->astOperand2() && tok->astOperand1()->str() == tok->astOperand2()->str())
                    return true;
            }
            return false;
        }
        std::string condition;
        std::string preprocessorDefinitions;
        std::string additionalIncludePaths;
    };
}

static std::list<std::string> toStringList(const std::string &s)
{
    std::list<std::string> ret;
    std::string::size_type pos1 = 0;
    std::string::size_type pos2;
    while ((pos2 = s.find(';',pos1)) != std::string::npos) {
        ret.push_back(s.substr(pos1, pos2-pos1));
        pos1 = pos2 + 1;
        if (pos1 >= s.size())
            break;
    }
    if (pos1 < s.size())
        ret.push_back(s.substr(pos1));
    return ret;
}

static void importPropertyGroup(const tinyxml2::XMLElement *node, std::map<std::string,std::string,cppcheck::stricmp> *variables, std::string *includePath, bool *useOfMfc)
{
    if (useOfMfc) {
        for (const tinyxml2::XMLElement *e = node->FirstChildElement(); e; e = e->NextSiblingElement()) {
            if (std::strcmp(e->Name(), "UseOfMfc") == 0) {
                *useOfMfc = true;
                break;
            }
        }
    }

    const char* labelAttribute = node->Attribute("Label");
    if (labelAttribute && std::strcmp(labelAttribute, "UserMacros") == 0) {
        for (const tinyxml2::XMLElement *propertyGroup = node->FirstChildElement(); propertyGroup; propertyGroup = propertyGroup->NextSiblingElement()) {
            const std::string name(propertyGroup->Name());
            const char *text = propertyGroup->GetText();
            (*variables)[name] = std::string(text ? text : "");
        }

    } else if (!labelAttribute) {
        for (const tinyxml2::XMLElement *propertyGroup = node->FirstChildElement(); propertyGroup; propertyGroup = propertyGroup->NextSiblingElement()) {
            if (std::strcmp(propertyGroup->Name(), "IncludePath") != 0)
                continue;
            const char *text = propertyGroup->GetText();
            if (!text)
                continue;
            std::string path(text);
            std::string::size_type pos = path.find("$(IncludePath)");
            if (pos != std::string::npos)
                path = path.substr(0,pos) + *includePath + path.substr(pos+14U);
            *includePath = path;
        }
    }
}

static void loadVisualStudioProperties(const std::string &props, std::map<std::string,std::string,cppcheck::stricmp> *variables, std::string *includePath, const std::string &additionalIncludeDirectories, std::list<ItemDefinitionGroup> &itemDefinitionGroupList)
{
    std::string filename(props);
    // variables can't be resolved
    if (!simplifyPathWithVariables(filename, *variables))
        return;

    // prepend project dir (if it exists) to transform relative paths into absolute ones
    if (!Path::isAbsolute(filename) && variables->count("ProjectDir") > 0)
        filename = Path::getAbsoluteFilePath(variables->at("ProjectDir") + filename);

    tinyxml2::XMLDocument doc;
    if (doc.LoadFile(filename.c_str()) != tinyxml2::XML_SUCCESS)
        return;
    const tinyxml2::XMLElement * const rootnode = doc.FirstChildElement();
    if (rootnode == nullptr)
        return;
    for (const tinyxml2::XMLElement *node = rootnode->FirstChildElement(); node; node = node->NextSiblingElement()) {
        if (std::strcmp(node->Name(), "ImportGroup") == 0) {
            const char *labelAttribute = node->Attribute("Label");
            if (labelAttribute == nullptr || std::strcmp(labelAttribute, "PropertySheets") != 0)
                continue;
            for (const tinyxml2::XMLElement *importGroup = node->FirstChildElement(); importGroup; importGroup = importGroup->NextSiblingElement()) {
                if (std::strcmp(importGroup->Name(), "Import") == 0) {
                    const char *projectAttribute = importGroup->Attribute("Project");
                    if (projectAttribute == nullptr)
                        continue;
                    std::string loadprj(projectAttribute);
                    if (loadprj.find('$') == std::string::npos) {
                        loadprj = Path::getPathFromFilename(filename) + loadprj;
                    }
                    loadVisualStudioProperties(loadprj, variables, includePath, additionalIncludeDirectories, itemDefinitionGroupList);
                }
            }
        } else if (std::strcmp(node->Name(),"PropertyGroup")==0) {
            importPropertyGroup(node, variables, includePath, nullptr);
        } else if (std::strcmp(node->Name(),"ItemDefinitionGroup")==0) {
            itemDefinitionGroupList.push_back(ItemDefinitionGroup(node, additionalIncludeDirectories));
        }
    }
}

void ImportProject::importVcxproj(const std::string &filename, std::map<std::string, std::string, cppcheck::stricmp> &variables, const std::string &additionalIncludeDirectories)
{
    variables["ProjectDir"] = Path::simplifyPath(Path::getPathFromFilename(filename));

    std::list<ProjectConfiguration> projectConfigurationList;
    std::list<std::string> compileList;
    std::list<ItemDefinitionGroup> itemDefinitionGroupList;
    std::string includePath;

    bool useOfMfc = false;

    tinyxml2::XMLDocument doc;
    tinyxml2::XMLError error = doc.LoadFile(filename.c_str());
    if (error != tinyxml2::XML_SUCCESS)
        return;
    const tinyxml2::XMLElement * const rootnode = doc.FirstChildElement();
    if (rootnode == nullptr)
        return;
    for (const tinyxml2::XMLElement *node = rootnode->FirstChildElement(); node; node = node->NextSiblingElement()) {
        if (std::strcmp(node->Name(), "ItemGroup") == 0) {
            const char *labelAttribute = node->Attribute("Label");
            if (labelAttribute && std::strcmp(labelAttribute, "ProjectConfigurations") == 0) {
                for (const tinyxml2::XMLElement *cfg = node->FirstChildElement(); cfg; cfg = cfg->NextSiblingElement()) {
                    if (std::strcmp(cfg->Name(), "ProjectConfiguration") == 0) {
                        ProjectConfiguration p(cfg);
                        if (p.platform != ProjectConfiguration::Unknown)
                            projectConfigurationList.push_back(ProjectConfiguration(cfg));
                    }
                }
            } else {
                for (const tinyxml2::XMLElement *e = node->FirstChildElement(); e; e = e->NextSiblingElement()) {
                    if (std::strcmp(e->Name(), "ClCompile") == 0) {
                        const char *include = e->Attribute("Include");
                        if (include && Path::acceptFile(include))
                            compileList.push_back(include);
                    }
                }
            }
        } else if (std::strcmp(node->Name(), "ItemDefinitionGroup") == 0) {
            itemDefinitionGroupList.push_back(ItemDefinitionGroup(node, additionalIncludeDirectories));
        } else if (std::strcmp(node->Name(), "PropertyGroup") == 0) {
            importPropertyGroup(node, &variables, &includePath, &useOfMfc);
        } else if (std::strcmp(node->Name(), "ImportGroup") == 0) {
            const char *labelAttribute = node->Attribute("Label");
            if (labelAttribute && std::strcmp(labelAttribute, "PropertySheets") == 0) {
                for (const tinyxml2::XMLElement *e = node->FirstChildElement(); e; e = e->NextSiblingElement()) {
                    if (std::strcmp(e->Name(), "Import") == 0) {
                        const char *projectAttribute = e->Attribute("Project");
                        if (projectAttribute)
                            loadVisualStudioProperties(projectAttribute, &variables, &includePath, additionalIncludeDirectories, itemDefinitionGroupList);
                    }
                }
            }
        }
    }

    for (std::list<std::string>::const_iterator c = compileList.begin(); c != compileList.end(); ++c) {
        for (std::list<ProjectConfiguration>::const_iterator p = projectConfigurationList.begin(); p != projectConfigurationList.end(); ++p) {
            FileSettings fs;
            fs.filename = Path::simplifyPath(Path::isAbsolute(*c) ? *c : Path::getPathFromFilename(filename) + *c);
            fs.cfg = p->name;
            fs.msc = true;
            fs.useMfc = useOfMfc;
            fs.defines = "_WIN32=1";
            if (p->platform == ProjectConfiguration::Win32)
                fs.platformType = cppcheck::Platform::Win32W;
            else if (p->platform == ProjectConfiguration::x64) {
                fs.platformType = cppcheck::Platform::Win64;
                fs.defines += ";_WIN64=1";
            }
            std::string additionalIncludePaths;
            for (std::list<ItemDefinitionGroup>::const_iterator i = itemDefinitionGroupList.begin(); i != itemDefinitionGroupList.end(); ++i) {
                if (!i->conditionIsTrue(*p))
                    continue;
                fs.defines += ';' + i->preprocessorDefinitions;
                additionalIncludePaths += ';' + i->additionalIncludePaths;
            }
            fs.setDefines(fs.defines);
            fs.setIncludePaths(Path::getPathFromFilename(filename), toStringList(includePath + ';' + additionalIncludePaths), variables);
            fileSettings.push_back(fs);
        }
    }
}

void ImportProject::importBcb6Prj(const std::string &projectFilename)
{
    tinyxml2::XMLDocument doc;
    tinyxml2::XMLError error = doc.LoadFile(projectFilename.c_str());
    if (error != tinyxml2::XML_SUCCESS)
        return;
    const tinyxml2::XMLElement * const rootnode = doc.FirstChildElement();
    if (rootnode == nullptr)
        return;

    const std::string& projectDir = Path::simplifyPath(Path::getPathFromFilename(projectFilename));

    std::list<std::string> compileList;
    std::string includePath;
    std::string userdefines;
    std::string sysdefines;
    std::string cflag1;

    for (const tinyxml2::XMLElement *node = rootnode->FirstChildElement(); node; node = node->NextSiblingElement()) {
        if (std::strcmp(node->Name(), "FILELIST") == 0) {
            for (const tinyxml2::XMLElement *f = node->FirstChildElement(); f; f = f->NextSiblingElement()) {
                if (std::strcmp(f->Name(), "FILE") == 0) {
                    const char *filename = f->Attribute("FILENAME");
                    if (filename && Path::acceptFile(filename))
                        compileList.push_back(filename);
                }
            }
        } else if (std::strcmp(node->Name(), "MACROS") == 0) {
            for (const tinyxml2::XMLElement *m = node->FirstChildElement(); m; m = m->NextSiblingElement()) {
                if (std::strcmp(m->Name(), "INCLUDEPATH") == 0) {
                    const char *v = m->Attribute("value");
                    if (v)
                        includePath = v;
                } else if (std::strcmp(m->Name(), "USERDEFINES") == 0) {
                    const char *v = m->Attribute("value");
                    if (v)
                       userdefines = v;
                } else if (std::strcmp(m->Name(), "SYSDEFINES") == 0) {
                    const char *v = m->Attribute("value");
                    if (v)
                        sysdefines = v;
                } else if (std::strcmp(m->Name(), "CFLAG1") == 0) {
                    const char *v = m->Attribute("value");
                    if (v)
                        cflag1 = v;
                }
            }
        }
    }

    std::set<std::string> cflags;

    {
        std::string arg;

        for (int i = 0; i < cflag1.size(); ++i) {
            if (cflag1.at(i) == ' ' && !arg.empty()) {
                cflags.insert(arg);
                arg.clear();
                continue;
            }
            arg += cflag1.at(i);
        }

        if (!arg.empty()) {
            cflags.insert(arg);
        }
    }
    
    std::string predefines;

    // Collecting predefines. See BCB6 help topic "Predefined macros"
    {
        //  Defined in any compiler that has an optimizer.
        predefines += ";__BCOPT__=1";

        // Version number.
        // BCB6 is 0x056? (SP4 is 0x0564)
        // @see http://docwiki.embarcadero.com/RADStudio/Tokyo/en/Predefined_Macros#C.2B.2B_Compiler_Versions_in_Predefined_Macros
        predefines += ";__BORLANDC__=0x0560";

        // Defined if Calling Convention is set to cdecl; otherwise undefined.
        // Check /PROJECT/OPTIONS/CFLAG1/@value neither contain -p, -pm, -pr nor -ps (-pc may or may not be there, as it is the default)
        bool useCdecl = (cflags.find("-p") == cflags.end()
            && cflags.find("-pm") == cflags.end()
            && cflags.find("-pr") == cflags.end()
            && cflags.find("-ps") == cflags.end());
        if (useCdecl) {
            predefines += ";__CDECL=1";
        }

        // Defined by default indicating that the default char is unsigned char. Use the -K compiler option to undefine this macro.
        // Check /PROJECT/OPTIONS/CFLAG1/@value contains -K
        bool treatCharAsUnsignedChar = (cflags.find("-K") != cflags.end());
        if (treatCharAsUnsignedChar) {
            predefines += ";_CHAR_UNSIGNED=1";
        }

        // Defined whenever one of the CodeGuard compiler options is used; otherwise it is undefined.
        // Check /PROJECT/OPTIONS/CFLAG1/@value contains -vGd, -vGt or -vGc
        bool codeguardUsed = (cflags.find("-vGd") != cflags.end()
            || cflags.find("-vGt") != cflags.end()
            || cflags.find("-vGc") != cflags.end());
        if (codeguardUsed) {
            predefines += ";__CODEGUARD__";
        }

        // When defined, the macro indicates that the program is a console application.
        // Check /PROJECT/OPTIONS/CFLAG1/@value contains -tWC
        bool isConsoleApp = (cflags.find("-tWC") != cflags.end());
        if (isConsoleApp) {
            predefines += ";__CONSOLE__=1";
        }

        // Enable stack unwinding. This is true by default; use -xd- to disable.
        // Check /PROJECT/OPTIONS/CFLAG1/@value doesn't contains -xd-
        bool enableStackUnwinding = (cflags.find("-xd-") == cflags.end());
        if (enableStackUnwinding) {
            predefines += ";_CPPUNWIND=1";
        }

        /*
        _ _cplusplus	1	Defined if in C++ mode; otherwise, undefined.

        _ _DLL_ _	1	Defined whenever the - WD compiler option is used; otherwise it is undefined.
        _ _FLAT_ _	1	Defined when compiling in 32 - bit flat memory model.

        _M_IX86	0x12c	Always defined.The default  value is 300. You can change the value to 400 or 500 by using the / 4 or /5 compiler options.
        _ _MT_ _	1	Defined only if the - tWM option is used.It specifies that the multithread library is to be linked.
        _ _PASCAL_ _ 	1	Defined if Calling Convention is set to Pascal; otherwise undefined.

        _ _STDC_ _	1	Defined if you compile with the - A compiler option; otherwise, it is undefined.
        _ _TCPLUSPLUS_ _ 	0x0560	Version number.
        _ _TEMPLATES_ _	1	Defined as 1 for C++ files(meaning that templates are supported); otherwise, it is undefined.
        _ _TLS_ _	1	Thread Local Storage.Always true in C++Builder.
        _ _TURBOC_ _ 	0x0560	Will increase in later releases.

        _WCHAR_T		Defined only for C++ programs to indicate that wchar_t is an intrinsically defined data type.
        _WCHAR_T_DEFINED		Defined only for C++ programs to indicate that wchar_t is an intrinsically defined data type.
        _Windows	 	Defined for Windows - only code.
        _ _WIN32_ _	1	Defined for console and GUI applications.
        */
    }

    // TODO
    // define:  __BCPLUSPLUS__
    // value:   0x0560 (0x0564 for our BCB6 SP4) @see http://docwiki.embarcadero.com/RADStudio/Tokyo/en/Predefined_Macros#C.2B.2B_Compiler_Versions_in_Predefined_Macros
    // desc:    Defined if you've selected C++ compilation; will increase in later releases.
    // 
    // C++ compilation is selected by file extension by default, so this define has to be set on a per-file base.
    // @see http://docwiki.embarcadero.com/RADStudio/Tokyo/en/BCC32.EXE,_the_C%2B%2B_32-bit_Command-Line_Compiler
    // > Files with the .CPP extension compile as C++ files. Files with a .C
    // > extension, with no extension, or with extensions other than .CPP,
    // > .OBJ, .LIB, or .ASM compile as C files.
    //
    // We can also force C++ compilation for all files. Check for the -P
    // command line switch being passed to the compiler.
    // /PROJECT/OPTIONS/CFLAG1/@style matches /["\s]-P["\s]/
    bool forceCppCompilation = (cflags.find("-P") != cflags.end());

    // Include paths may contain variables like "$(BCB)\include" or "$(BCB)\include\vcl".
    // Those get resolved by ImportProject::FileSettings::setIncludePaths by
    // 1. checking the provided variables map ("BCB" => "C:\\Program Files (x86)\\Borland\\CBuilder6")
    // 2. checking env variables as a fallback
    // Setting env is always possible. Configuring the variables via cli might be an addition.
    // Reading the BCB6 install location from registry in windows environments would also be possible,
    // but I didn't see any such functionality around the source. Not in favor of adding it only
    // for the BCB6 project loading.
    std::map<std::string, std::string, cppcheck::stricmp> variables;
    std::string defines;

    if (!predefines.empty()) {
       defines = defines.empty() ? predefines : (defines + ";" + predefines);
    }

    if (!userdefines.empty()) {
       defines = defines.empty() ? userdefines : (defines + ";" + userdefines);
    }
        
    if (!sysdefines.empty()) {
        defines = defines.empty() ? sysdefines : (defines + ";" + sysdefines);
    }

    for (std::list<std::string>::const_iterator c = compileList.begin(); c != compileList.end(); ++c) {
        // TODO add define __BCPLUSPLUS__ depending on forceCppCompilation and file extension
        FileSettings fs;
        fs.setIncludePaths(projectDir, toStringList(includePath), variables);
        fs.setDefines(defines);
        fs.filename = Path::simplifyPath(Path::isAbsolute(*c) ? *c : projectDir + *c);
        fileSettings.push_back(fs);
    }
}