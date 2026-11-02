#pragma once

#include <cctype>
#include <string>

namespace vcore {

inline std::string normalizedLanguage(const std::string& language) {
    std::string normalized;
    normalized.reserve(language.size());
    for (unsigned char ch : language) {
        if (ch == '_' || ch == ' ') {
            normalized.push_back('-');
            continue;
        }
        normalized.push_back(static_cast<char>(std::tolower(ch)));
    }
    return normalized;
}

inline bool isEnglishLanguage(const std::string& language) {
    const std::string normalized = normalizedLanguage(language);
    return normalized == "en" || normalized.rfind("en-", 0) == 0;
}

// ASR bias only. Grok accepts "zh" and "en"; regional Spanish/Portuguese
// codes are not used by this app.
inline std::string realtimeLanguageHint(const std::string& language) {
    return isEnglishLanguage(language) ? "en" : "zh";
}

// Grok Voice has no output-language field. The prompting guide locks the
// spoken language in instructions, under Voice & Communication Style.
inline std::string realtimeOutputInstructions(const std::string& language) {
    if (isEnglishLanguage(language)) {
        return "## Role & Persona\n"
               "You are a realtime voice assistant.\n"
               "\n"
               "## Objective\n"
               "Answer the caller briefly and helpfully.\n"
               "\n"
               "## Conversation Flow\n"
               "Listen, answer, then wait for the next question.\n"
               "\n"
               "## Guardrails & Escalation\n"
               "If you do not know, say so briefly. Do not invent facts.\n"
               "\n"
               "## Voice & Communication Style\n"
               "- Spoken words only. One or two short sentences per turn. Do not use long paragraphs.\n"
               "- Respond only in English.\n"
               "- Speech and text must both be English.\n"
               "- If the caller speaks another language, continue in English.\n"
               "\n"
               "## CRITICAL INSTRUCTIONS\n"
               "ALWAYS respond only in English. NEVER switch language.";
    }
    return "## Role & Persona\n"
           "你是实时语音助手。\n"
           "\n"
           "## Objective\n"
           "用简体中文简短回答用户的问题。\n"
           "\n"
           "## Conversation Flow\n"
           "听完后直接回答，然后等待下一句。\n"
           "\n"
           "## Guardrails & Escalation\n"
           "不知道就简短说明，不要编造。\n"
           "\n"
           "## Voice & Communication Style\n"
           "- 只说口语，每次一两句，不要长段落。\n"
           "- 全程只使用简体中文。\n"
           "- 语音和文字都必须是简体中文。\n"
           "- 即使用户说其他语言，也继续用简体中文回答。\n"
           "\n"
           "## CRITICAL INSTRUCTIONS\n"
           "ALWAYS 只用简体中文回答。NEVER 输出其他语言。";
}

}  // namespace vcore
