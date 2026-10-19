#pragma once

#include "json.hpp"

namespace deckcalc {

// 激奏（Gekisou / Fever Mission）算分与组队搜索接口。
// 算法与状态机移植自 haneoka-gakuen/haneoka (https://github.com/haneoka-gakuen/haneoka)，在此致谢。
Json handle_gekisou_score(const Json &input);
Json handle_gekisou_rank(const Json &input);

} // namespace deckcalc
