#include "gekisou_internal.hpp"

#include <numeric>

namespace deckcalc {
namespace gekisou_detail {
namespace {

// 整曲逐帧状态机：Combo、判定改判、体力、分数差分与激奏控制器
class ComboCounter {
public:
    void add_judgement(int32_t time_ms, int32_t judgement) {
        if (judgement == 0 || judgement == 7)
            return;
        int32_t order = add_order_++;
        size_t i = times_.size();
        while (i >= 1 &&
               (time_ms < times_[i - 1] || (time_ms == times_[i - 1] && order < orders_[i - 1]))) {
            --i;
        }
        times_.insert(times_.begin() + static_cast<ptrdiff_t>(i), time_ms);
        orders_.insert(orders_.begin() + static_cast<ptrdiff_t>(i), order);
        judgements_.insert(judgements_.begin() + static_cast<ptrdiff_t>(i), judgement);
        combos_.insert(combos_.begin() + static_cast<ptrdiff_t>(i), 0);
        int32_t combo = (i == 0) ? 0 : combos_[i - 1];
        for (size_t k = i; k < times_.size(); ++k) {
            int32_t j = judgements_[k];
            combo = (j == 1 || j == 2) ? 0 : (combo + 1);
            combos_[k] = combo;
        }
    }

    int32_t timing_combo(int32_t t) const {
        size_t n = times_.size();
        if (n == 0 || times_[0] >= t)
            return 0;
        size_t lo = 0;
        size_t hi = n - 1;
        while (lo < hi) {
            size_t mid = lo + (hi - lo + 1) / 2;
            if (times_[mid] < t)
                lo = mid;
            else
                hi = mid - 1;
        }
        return combos_[lo];
    }

private:
    std::vector<int32_t> times_;
    std::vector<int32_t> orders_;
    std::vector<int32_t> judgements_;
    std::vector<int32_t> combos_;
    int32_t add_order_ = 0;
};

constexpr int32_t kStay = 0;
constexpr int32_t kExecuteFrame = 2;
constexpr int32_t kExecuting = 3;
constexpr int32_t kEndFrame = 4;
constexpr int32_t kOneShot = 1;
constexpr int32_t kSustained = 2;

struct ConvertEffect {
    int32_t type = 0;
    int32_t value = 0;
    int32_t limit_count = 0;
    std::vector<int32_t> targets;
    int32_t effect_id = 0;
};

class Conversion {
public:
    explicit Conversion(std::unordered_set<int32_t> no_just) : no_just_(std::move(no_just)) {}

    int32_t convert(int32_t judgement, int32_t judgement_type, int32_t time_ms) {
        for (size_t i = 0; i < order_.size(); ++i) {
            int32_t key = order_[i];
            if (!registered_.count(key))
                continue;
            int32_t out = try_convert(key, judgement, judgement_type, time_ms);
            if (out != judgement)
                return out;
            if (!(i < order_.size() && order_[i] == key)) {
                if (i > 0)
                    --i;
                else {
                    // order_[0] was removed; restart at 0 on next iteration
                    i = static_cast<size_t>(-1);
                }
            }
        }
        return judgement;
    }

    std::optional<int32_t> update(const std::string &key, int32_t state,
                                  const ConvertEffect &effect) {
        std::optional<int32_t> finished;
        auto fit = limit_finished_.find(key);
        if (fit != limit_finished_.end()) {
            finished = fit->second;
            limit_finished_.erase(fit);
            state = kEndFrame;
        }
        if (state == kEndFrame) {
            auto it = id_map_.find(key);
            if (it != id_map_.end()) {
                int32_t id = it->second;
                id_map_.erase(it);
                unregister(id);
                params_.erase(id);
            }
        } else if (state == kExecuteFrame) {
            auto cit = context_.find(effect.effect_id);
            if (cit == context_.end()) {
                cit = context_.emplace(effect.effect_id, effect.targets).first;
            }
            int32_t to = (effect.type == 13005)
                             ? 6
                             : (effect.value >= 1 && effect.value <= 6 ? effect.value : -1);
            int32_t id = reg();
            id_map_[key] = id;
            params_[id] = {key, 0, effect.limit_count, cit->second, to};
        }
        return finished;
    }

private:
    struct ConvertParam {
        std::string key;
        int32_t count = 0;
        int32_t max_count = 0;
        std::vector<int32_t> targets;
        int32_t to = -1;
    };

    int32_t reg() {
        int32_t k = ++func_id_;
        registered_.insert(k);
        order_.insert(order_.begin(), k);
        return k;
    }

    void unregister(int32_t id) {
        registered_.erase(id);
        auto it = std::find(order_.begin(), order_.end(), id);
        if (it != order_.end())
            order_.erase(it);
    }

    int32_t try_convert(int32_t id, int32_t cur, int32_t judgement_type, int32_t time_ms) {
        auto pit = params_.find(id);
        if (pit == params_.end())
            return cur;
        auto &p = pit->second;
        if (p.to == -1 || cur == p.to ||
            std::find(p.targets.begin(), p.targets.end(), cur) == p.targets.end())
            return cur;
        if (p.to == 6 && no_just_.count(judgement_type))
            return cur;
        const int32_t to = p.to;
        p.count++;
        if (p.max_count > 0 && p.max_count <= p.count) {
            std::string k = p.key;
            unregister(id);
            id_map_.erase(k);
            params_.erase(id);
            limit_finished_[k] = time_ms;
        }
        return to;
    }

    std::unordered_set<int32_t> no_just_;
    std::vector<int32_t> order_;
    std::unordered_set<int32_t> registered_;
    int32_t func_id_ = 0;
    std::unordered_map<std::string, int32_t> id_map_;
    std::unordered_map<int32_t, ConvertParam> params_;
    std::unordered_map<std::string, int32_t> limit_finished_;
    std::unordered_map<int32_t, std::vector<int32_t>> context_;
};

class LifeController {
public:
    LifeController(int32_t initial_life, std::array<int32_t, 7> damage, int32_t music_length_ms)
        : current_life(initial_life), initial_life_(initial_life), damage_(damage),
          max_life_(initial_life), max_frame_(wrap_add(get_frame(music_length_ms), 2)),
          commands_(std::max(1, max_frame_)) {}

    int32_t maximum() const {
        return max_life_;
    }

    void note_damage(int32_t time_ms, int32_t judgement) {
        if (judgement <= 0 || judgement >= 7)
            return;
        int32_t d = damage_[judgement];
        if (d > 0)
            add({time_ms, 0, d, false, false});
    }

    void recovery(int32_t time_ms, int32_t amount, bool over_heal) {
        if (amount > 0)
            add({time_ms, 2, amount, over_heal, false});
    }

    void skill_damage(int32_t time_ms, int32_t amount, bool safety) {
        if (amount > 0)
            add({time_ms, 1, amount, false, safety});
    }

    int32_t enable_reduction(int32_t time_ms, int32_t bp) {
        int32_t id = ++reduction_counter_;
        reductions_[id] = bp;
        add({time_ms, 5, bp, false, false});
        return id;
    }

    void disable_reduction(int32_t time_ms, int32_t id) {
        auto it = reductions_.find(id);
        if (it == reductions_.end())
            return;
        int32_t bp = it->second;
        reductions_.erase(it);
        add({time_ms, 6, bp, false, false});
    }

    int32_t add_limit(int32_t amount) {
        int32_t id = ++limit_counter_;
        limits_[id] = amount;
        update_max();
        return id;
    }

    void subtract_limit(int32_t id) {
        if (limits_.erase(id))
            update_max();
    }

    int32_t enable_guard(int32_t time_ms) {
        int32_t id = ++guard_counter_;
        guards_.push_back(id);
        add({time_ms, 3, 0, false, false});
        return id;
    }

    void disable_guard(int32_t time_ms, int32_t id) {
        auto it = std::find(guards_.begin(), guards_.end(), id);
        if (it == guards_.end())
            return;
        guards_.erase(it);
        add({time_ms, 4, 0, false, false});
    }

    int32_t life_at(int32_t ms) {
        int32_t frame = frame_of(ms);
        int32_t c = cached_frame_;
        bool full = false;
        int32_t start = 0;
        std::array<int32_t, 3> state{initial_life_, 0, 0};
        if (c < 0 || frame <= c) {
            full = (c >= 0 && frame <= c);
            start = 0;
        } else {
            full = false;
            state = {cached_life_, cached_guard_, cached_reduction_};
            start = c + 1;
        }
        for (int32_t k = start; k < std::max(frame, start); ++k) {
            for (const auto &cmd : commands_[k])
                step(state, cmd);
        }
        if (frame < 0)
            return state[0];
        if (!full) {
            cached_frame_ = frame - 1;
            cached_life_ = state[0];
            cached_guard_ = state[1];
            cached_reduction_ = state[2];
        }
        if (frame >= max_frame_)
            return state[0];
        for (const auto &cmd : commands_[frame]) {
            if (cmd.time_ms > ms)
                break;
            step(state, cmd);
        }
        return state[0];
    }

    void sync_current(int32_t t) {
        current_life = life_at(t);
    }

    int32_t current_life = 0;

private:
    struct LifeCommand {
        int32_t time_ms = 0;
        int32_t kind = 0;
        int32_t value = 0;
        bool over_heal = false;
        bool safety = false;
    };

    static int32_t reduce_dmg(int32_t damage, int32_t bp) {
        if (bp < 1)
            return damage;
        if ((static_cast<uint32_t>(bp) >> 4) > 0x270u)
            return 0;
        int32_t r = wrap_mul(wrap_sub(10000, bp), damage) / 10000;
        return std::max(0, r);
    }

    int32_t frame_of(int32_t ms) const {
        int32_t frame = get_frame(ms);
        return max_frame_ <= frame ? (max_frame_ - 1) : frame;
    }

    void add(const LifeCommand &cmd) {
        int32_t frame = frame_of(cmd.time_ms);
        if (frame < 0)
            frame = 0;
        auto &list = commands_[frame];
        size_t i = list.size();
        while (i >= 1 && cmd.time_ms < list[i - 1].time_ms)
            --i;
        list.insert(list.begin() + static_cast<ptrdiff_t>(i), cmd);
        if (frame <= cached_frame_) {
            cached_frame_ = -1;
            cached_life_ = cached_guard_ = cached_reduction_ = 0;
        }
    }

    void update_max() {
        int32_t sum = 0;
        for (const auto &[_, v] : limits_)
            sum = wrap_add(sum, v);
        max_life_ = wrap_add(initial_life_, sum);
    }

    void step(std::array<int32_t, 3> &state, const LifeCommand &cmd) const {
        int32_t life = state[0];
        int32_t guard = state[1];
        int32_t reduction = state[2];
        switch (cmd.kind) {
        case 0: // NOTE_DAMAGE
            if (guard < 1)
                life = std::max(0, wrap_sub(life, reduce_dmg(cmd.value, reduction)));
            break;
        case 1: // SKILL_DAMAGE
            if (guard < 1 && (life > 0 || !cmd.safety)) {
                life = wrap_sub(life, reduce_dmg(cmd.value, reduction));
                life = std::max(life, cmd.safety ? 1 : 0);
            }
            break;
        case 2: // RECOVERY
            if (life > 0) {
                int32_t cap = cmd.over_heal ? (max_life_ << 1) : max_life_;
                int32_t s = wrap_add(cmd.value, life);
                life = std::min(s, cap);
            }
            break;
        case 3: // GUARD_START
            guard = wrap_add(guard, 1);
            break;
        case 4: // GUARD_END
            guard = std::max(0, wrap_sub(guard, 1));
            break;
        case 5: // REDUCTION_START
            reduction = wrap_add(reduction, cmd.value);
            break;
        case 6: // REDUCTION_END
            reduction = std::max(0, wrap_sub(reduction, cmd.value));
            break;
        }
        state = {life, guard, reduction};
    }

    int32_t initial_life_;
    std::array<int32_t, 7> damage_;
    int32_t max_life_;
    int32_t max_frame_;
    std::vector<std::vector<LifeCommand>> commands_;
    std::vector<int32_t> guards_;
    int32_t guard_counter_ = 0;
    std::unordered_map<int32_t, int32_t> reductions_;
    int32_t reduction_counter_ = 0;
    std::unordered_map<int32_t, int32_t> limits_;
    int32_t limit_counter_ = 0;
    int32_t cached_frame_ = -1;
    int32_t cached_life_ = 0;
    int32_t cached_guard_ = 0;
    int32_t cached_reduction_ = 0;
};

struct FactorCommand {
    int32_t time_ms = 0;
    int32_t owner = 0;
    int32_t note = 0;
    int32_t combo = 0;
    int32_t judgement = 0;
    int32_t judge = 0;
    int32_t luck = 0;
};

struct FactorState {
    float combo = 0.0f;
    float note = 1.0f;
    float just = 0.0f;
    float perfect = 0.0f;
    float great = 0.0f;
    float good = 0.0f;
    int32_t luck = 0;

    float judgement_factor(int32_t score_type) const {
        switch (score_type) {
        case 1:
            return just;
        case 2:
            return perfect;
        case 3:
            return great;
        case 4:
            return good;
        default:
            return 0.0f;
        }
    }

    void apply(const FactorCommand &cmd) {
        float fc = mill_to_f32(cmd.combo);
        float fn = mill_to_f32(cmd.note);
        float fj = mill_to_f32(cmd.judge);
        if (fc != 0.0f)
            combo = f32(combo + fc);
        if (fn != 0.0f)
            note = f32(note + fn);
        switch (cmd.judgement) {
        case 6:
            just = f32(just + fj);
            break;
        case 5:
            perfect = f32(perfect + fj);
            break;
        case 4:
            great = f32(great + fj);
            break;
        case 3:
            good = f32(good + fj);
            break;
        }
        luck = wrap_add(luck, cmd.luck);
    }
};

struct FrameDiff {
    float combo = 0.0f;
    float note = 0.0f;
    float just = 0.0f;
    float perfect = 0.0f;
    float great = 0.0f;
    float good = 0.0f;
    int32_t luck = 0;

    void add(const FactorCommand &cmd) {
        luck = wrap_add(luck, cmd.luck);
        float fc = mill_to_f32(cmd.combo);
        float fn = mill_to_f32(cmd.note);
        float fj = mill_to_f32(cmd.judge);
        if (fc != 0.0f)
            combo = f32(combo + fc);
        if (fn != 0.0f)
            note = f32(note + fn);
        switch (cmd.judgement) {
        case 6:
            just = f32(just + fj);
            break;
        case 5:
            perfect = f32(perfect + fj);
            break;
        case 4:
            great = f32(great + fj);
            break;
        case 3:
            good = f32(good + fj);
            break;
        }
    }

    void undo(FactorState &s) {
        s.just = f32(s.just - just);
        s.perfect = f32(s.perfect - perfect);
        s.combo = f32(s.combo - combo);
        s.note = f32(s.note - note);
        s.luck = wrap_sub(s.luck, luck);
        s.great = f32(s.great - great);
        s.good = f32(s.good - good);
        combo = note = just = perfect = great = good = 0.0f;
        luck = 0;
    }
};

struct GekisouComboInfo {
    virtual ~GekisouComboInfo() = default;
    virtual std::optional<int32_t> gekisou_combo(int32_t time_ms) const = 0;
};

using LiveData = EngineMaster::Live;

class ScoreCalculator {
public:
    FactorState state;
    const LiveData &settings;
    double power;
    float level_factor;
    int32_t converted_count;
    float assist;
    std::function<void(int32_t, int32_t, float, float, float, float)> on_note;

    ScoreCalculator(const LiveData &settings_in, double power_in, int32_t level,
                    int32_t converted_count_in, float assist_in = 1.0f)
        : settings(settings_in), power(power_in),
          level_factor(f32(f32(f32(wrap_sub(level, 5)) * f32(0.005)) + 1.0f)),
          converted_count(converted_count_in), assist(assist_in) {}

    float table(int32_t type, int32_t combo) const {
        auto it = settings.combo.find(type);
        if (it == settings.combo.end() || it->second.empty() || it->second[0].first > combo)
            return 0.0f;
        const auto &rows = it->second;
        size_t lo = 0;
        size_t hi = rows.size() - 1;
        while (lo < hi) {
            size_t mid = lo + (hi - lo + 1) / 2;
            if (rows[mid].first > combo)
                hi = mid - 1;
            else
                lo = mid;
        }
        return rows[lo].second;
    }

    float gekisou_combo_factor(const GekisouComboInfo *info, int32_t time_ms) const {
        if (!info)
            return 1.0f;
        auto c = info->gekisou_combo(time_ms);
        if (!c.has_value())
            return 1.0f;
        return f32(min_ignoring_nan(table(1, *c), 1.0f) + 1.0f);
    }

    int32_t note_score(int32_t combo, int32_t life, int32_t time_ms, int32_t op, int32_t score_type,
                       const GekisouComboInfo *info, int32_t note_id = 0) {
        float cum = table(0, combo);
        float gk = gekisou_combo_factor(info, time_ms);
        float jf = state.judgement_factor(score_type);
        float score_up = f32(state.note + jf);
        int32_t luck_percent = std::min(wrap_add(state.luck, 100), 200);
        float luck = f32(f32(luck_percent) / 100.0f);
        float combo_factor = f32(gk * f32(state.combo + f32(min_ignoring_nan(cum, 1.0f) + 1.0f)));
        if (on_note)
            on_note(note_id, score_type, state.note, gk, luck, jf);
        return note_score_core(life, op, score_type, combo_factor, score_up, luck);
    }

    int32_t note_score_core(int32_t life, int32_t op, int32_t score_type, float combo_factor,
                            float score_up, float luck) const {
        int32_t note_pct = settings.note_percent.at(op);
        int32_t judge_pct = settings.judge_percent.at(score_type);
        float life_factor = life > 0 ? 1.0f : settings.life_onus;
        float t = f32(f32(settings.adjustment * f32(power)) * level_factor);
        float a = f32(f32(f32(note_pct) / 100.0f) * t);
        float b = f32(f32(f32(judge_pct) / 100.0f) * a);
        float c = f32(f32(b * combo_factor) * score_up);
        float d = f32(luck * c);
        float x = f32(d / f32(converted_count));
        double floored = std::floor(static_cast<double>(x));
        float y = (floored == std::numeric_limits<double>::infinity()) ? f32(kI32Min)
                                                                       : f32(sat_i32(floored));
        float z = f32(assist * f32(life_factor * y));
        return floor_to_i32(z);
    }
};

struct NoteCommand {
    int32_t time_ms = 0;
    int32_t life = 0;
    int32_t note_id = 0;
    int32_t op = 0;
    int32_t score_type = 0;
    int32_t added = 0;
};

class IncrementalScore {
public:
    ScoreCalculator calc;
    int32_t max_frame;
    int32_t score = 0;
    int32_t rank_bonus = 0;

    IncrementalScore(ScoreCalculator calc_in, int32_t music_length_ms)
        : calc(std::move(calc_in)), max_frame(std::max(1, get_frame(music_length_ms) + 50)),
          notes_(max_frame), factors_(max_frame), diffs_(max_frame) {}

    void add_note(int32_t time_ms, int32_t life, int32_t note_id, int32_t op, int32_t score_type) {
        int32_t frame = file(time_ms);
        notes_[frame].push_back({time_ms, life, note_id, op, score_type, 0});
    }

    void add_factor(const FactorCommand &cmd) {
        int32_t frame = file(cmd.time_ms);
        factors_[frame].push_back(cmd);
    }

    void add_fixed(int32_t time_ms, int32_t s) {
        pending_fixed_ = std::make_pair(time_ms, s);
    }

    int32_t calculate(int32_t t, const ComboCounter &combo, const GekisouComboInfo *info) {
        int32_t g = get_frame(t);
        int32_t to = g < 0 ? 0 : g;
        if (max_frame <= g)
            to = max_frame - 1;
        int32_t u = added_ < 0 ? to : std::min(to, added_ - 1);
        int32_t start = 0;
        if (u < prev_) {
            for (int32_t frame = prev_; frame > u; --frame)
                undo(frame);
            start = u + 1;
        } else {
            start = prev_ + 1;
        }
        for (int32_t frame = start; frame <= to; ++frame)
            execute(frame, combo, info);
        if (pending_fixed_.has_value()) {
            auto [ft, fs] = *pending_fixed_;
            pending_fixed_.reset();
            int32_t ff = get_frame(ft);
            fixed_[ff] = fs;
            score = wrap_add(score, fs);
        }
        prev_ = to;
        added_ = -1;
        return score;
    }

private:
    int32_t file(int32_t t) {
        int32_t frame = get_frame(t);
        if (frame < 0)
            frame = 0;
        if (max_frame <= frame)
            frame = max_frame - 1;
        added_ = (added_ < 0) ? frame : std::min(added_, frame);
        return frame;
    }

    void undo(int32_t frame) {
        for (const auto &n : notes_[frame])
            score = wrap_sub(score, n.added);
        if (auto it = fixed_.find(frame); it != fixed_.end())
            score = wrap_sub(score, it->second);
        diffs_[frame].undo(calc.state);
    }

    void execute(int32_t frame, const ComboCounter &combo, const GekisouComboInfo *info) {
        const auto &fl = factors_[frame];
        auto &nl = notes_[frame];
        if (fl.empty() && nl.empty()) {
            if (auto it = fixed_.find(frame); it != fixed_.end()) {
                score = wrap_add(score, it->second);
                rank_bonus = wrap_add(rank_bonus, it->second);
            }
            return;
        }
        std::vector<size_t> of_idx(fl.size());
        std::iota(of_idx.begin(), of_idx.end(), 0);
        if (fl.size() > 1) {
            std::sort(of_idx.begin(), of_idx.end(), [&](size_t a, size_t b) {
                if (fl[a].time_ms != fl[b].time_ms)
                    return fl[a].time_ms < fl[b].time_ms;
                if (fl[a].owner != fl[b].owner)
                    return fl[a].owner < fl[b].owner;
                return a < b;
            });
        }
        std::vector<size_t> on_idx(nl.size());
        std::iota(on_idx.begin(), on_idx.end(), 0);
        if (nl.size() > 1) {
            std::sort(on_idx.begin(), on_idx.end(), [&](size_t a, size_t b) {
                if (nl[a].time_ms != nl[b].time_ms)
                    return nl[a].time_ms < nl[b].time_ms;
                if (nl[a].note_id != nl[b].note_id)
                    return nl[a].note_id < nl[b].note_id;
                return a < b;
            });
        }
        size_t a = 0;
        size_t b = 0;
        while (a < fl.size() || b < nl.size()) {
            if (a < fl.size() &&
                (b >= nl.size() || nl[on_idx[b]].time_ms >= fl[of_idx[a]].time_ms)) {
                const auto &c = fl[of_idx[a]];
                calc.state.apply(c);
                diffs_[frame].add(c);
                ++a;
            } else {
                auto &n = nl[on_idx[b]];
                int32_t s = calc.note_score(combo.timing_combo(n.time_ms), n.life, n.time_ms, n.op,
                                            n.score_type, info, n.note_id);
                n.added = s;
                score = wrap_add(score, s);
                ++b;
            }
        }
        if (auto it = fixed_.find(frame); it != fixed_.end()) {
            score = wrap_add(score, it->second);
            rank_bonus = wrap_add(rank_bonus, it->second);
        }
    }

    std::vector<std::vector<NoteCommand>> notes_;
    std::vector<std::vector<FactorCommand>> factors_;
    std::vector<FrameDiff> diffs_;
    int32_t prev_ = -1;
    int32_t added_ = -1;
    std::unordered_map<int32_t, int32_t> fixed_;
    std::optional<std::pair<int32_t, int32_t>> pending_fixed_;
};

// 激奏（Gekisou）抽奖机、槽位与区间状态机
std::optional<int32_t> lottery_table(const std::vector<LotItem> &items, int32_t r) {
    if (items.empty())
        return std::nullopt;
    int32_t total = 0;
    for (const auto &it : items)
        total = wrap_add(total, it.first);
    if (total < 1)
        return std::nullopt;
    int32_t abs_r = (r == kI32Min) ? kI32Min : std::abs(r);
    int32_t m = srem(abs_r, total);
    int32_t cum = 0;
    for (const auto &it : items) {
        cum = wrap_add(cum, it.first);
        if (m < cum)
            return it.second;
    }
    return std::nullopt;
}

struct LuckSkillTable {
    std::vector<LotItem> items;
    float buff = 0.0f;
    int32_t total = 0;

    LuckSkillTable() = default;
    explicit LuckSkillTable(std::vector<LotItem> in_items) : items(std::move(in_items)) {
        std::sort(items.begin(), items.end(),
                  [](const LotItem &a, const LotItem &b) { return b.second < a.second; });
        for (const auto &it : items)
            total = wrap_add(total, it.first);
    }

    std::optional<int32_t> lottery(int32_t r) const {
        if (items.empty())
            return std::nullopt;
        int32_t abs_r = (r == kI32Min) ? kI32Min : std::abs(r);
        int32_t m = srem(abs_r, total);
        int32_t cum = 0;
        for (const auto &it : items) {
            cum = wrap_add(cum, buffed_weight(buff, it.first));
            if (m < cum)
                return it.second;
        }
        return std::nullopt;
    }

    std::optional<int32_t> lottery_with_minimum(int32_t r, int32_t minimum) const {
        if (items.empty())
            return std::nullopt;
        int32_t n = 0;
        int32_t inc = 0;
        int32_t exc = 0;
        for (const auto &it : items) {
            int32_t w = buffed_weight(buff, it.first);
            if (minimum <= it.second) {
                ++n;
                inc = wrap_add(inc, w);
            } else {
                exc = wrap_add(exc, w);
            }
        }
        if (n == 0)
            return std::nullopt;
        int32_t per = sdiv(exc, n);
        int32_t tot = wrap_add(inc, wrap_mul(per, n));
        int32_t abs_r = (r == kI32Min) ? kI32Min : std::abs(r);
        int32_t m = srem(abs_r, tot);
        int32_t cum = 0;
        for (const auto &it : items) {
            if (minimum <= it.second) {
                cum = wrap_add(cum, wrap_add(per, buffed_weight(buff, it.first)));
                if (m < cum)
                    return it.second;
            }
        }
        return std::nullopt;
    }
};

class LotteryMachine {
public:
    explicit LotteryMachine(const GekisouTables &t) {
        for (const auto &r : t.luck_base_points) {
            if (r.weight <= 0)
                continue;
            LotItem it{r.weight, r.point};
            if (r.category == 0) {
                if (r.judgement == 3)
                    good_.push_back(it);
                else if (r.judgement == 4)
                    great_.push_back(it);
                else if (r.judgement == 5)
                    perfect_.push_back(it);
            } else if (r.category == 1 && r.judgement == 5) {
                hold_.push_back(it);
            }
        }
        std::array<std::vector<LotItem>, 5> bonus;
        for (const auto &r : t.luck_bonus_lots) {
            if (r.weight > 0 && r.lot_type >= 0 && r.lot_type < 5)
                bonus[r.lot_type].emplace_back(r.weight, r.result);
        }
        for (size_t i = 0; i < 5; ++i)
            tables_[i] = LuckSkillTable(std::move(bonus[i]));
    }

    void set_buff(int32_t percent) {
        float mul = f32(f32(percent) / 100.0f);
        for (auto &t : tables_)
            t.buff = mul;
    }

    int32_t enable_minimum(int32_t result, int32_t limit) {
        minimum_counter_ = wrap_add(minimum_counter_, 1);
        minimum_.push_back({minimum_counter_, result, limit >= 1 ? limit : -1});
        return minimum_counter_;
    }

    void disable_minimum(int32_t id) {
        minimum_.erase(std::remove_if(minimum_.begin(), minimum_.end(),
                                      [&](const std::array<int32_t, 3> &e) { return e[0] == id; }),
                       minimum_.end());
    }

    bool is_minimum_active(int32_t id) const {
        for (const auto &e : minimum_)
            if (e[0] == id)
                return true;
        return false;
    }

    int32_t take_last_consumed(int32_t id) {
        auto it = last_consumed_.find(id);
        if (it == last_consumed_.end())
            return -1;
        int32_t v = it->second;
        last_consumed_.erase(it);
        return v;
    }

    int32_t luck_bonus(int32_t lot_type, int32_t t, LiveRandom &random) {
        int32_t r = random.next_int(LiveRandom::kLuck);
        int32_t applied = max_minimum();
        const auto &table = tables_.at(lot_type);
        if (applied < 1) {
            return table.lottery(r).value_or(0);
        }
        int32_t v = table.lottery_with_minimum(r, applied).value_or(0);
        consume_minimum(applied, t);
        return v;
    }

    int32_t base_point(int32_t nt, int32_t j, LiveRandom &random) {
        if (is_non_luck_note(nt))
            return 0;
        uint32_t u = static_cast<uint32_t>(j + 1);
        if (u < 9 && ((0x107u >> (u & 31u)) & 1u) != 0)
            return 0;
        int32_t r = random.next_int(LiveRandom::kLuck);
        const std::vector<LotItem> *items = nullptr;
        if (is_sub_note(nt))
            items = &hold_;
        else if (static_cast<uint32_t>(j - 5) < 2u)
            items = &perfect_;
        else if (j == 4)
            items = &great_;
        else if (j == 3)
            items = &good_;
        else
            return 0;
        return lottery_table(*items, r).value_or(0);
    }

private:
    int32_t max_minimum() const {
        int32_t m = 0;
        for (const auto &e : minimum_)
            if (m <= e[1])
                m = e[1];
        return m;
    }

    void consume_minimum(int32_t applied, int32_t t) {
        std::vector<std::array<int32_t, 3>> next;
        for (auto e : minimum_) {
            if (e[1] > applied || e[2] < 0) {
                next.push_back(e);
                continue;
            }
            e[2] = wrap_sub(e[2], 1);
            if (e[2] != 0) {
                next.push_back(e);
                continue;
            }
            last_consumed_[e[0]] = t;
        }
        minimum_ = std::move(next);
    }

    std::vector<LotItem> good_;
    std::vector<LotItem> great_;
    std::vector<LotItem> perfect_;
    std::vector<LotItem> hold_;
    std::array<LuckSkillTable, 5> tables_;
    std::vector<std::array<int32_t, 3>> minimum_;
    int32_t minimum_counter_ = 0;
    std::unordered_map<int32_t, int32_t> last_consumed_;
};

struct LuckScore {
    int32_t total_bonus_point = 0;
    int32_t gauge = 0;
    int32_t lot_count = 0;
    int32_t rush_combo = 0;
    std::array<int32_t, 4> results{0, 0, 0, 0};
    int32_t next = -1;
    int32_t gauge_max = 100;
    int32_t gauge_max_default = 100;
    int32_t gauge_max_rush = 50;

    void add_gauge(int32_t v) {
        int32_t g = wrap_add(gauge, v);
        int32_t m = gauge_max;
        if (m <= g) {
            lot_count = wrap_add(lot_count, g / m);
            gauge = g % m;
        } else {
            gauge = g;
        }
    }

    void change_max(int32_t m) {
        if (m < 1 || gauge_max == m)
            return;
        gauge_max = m;
        int32_t g = gauge;
        if (g <= m)
            return;
        lot_count = wrap_add(lot_count, g / m);
        gauge = g % m;
    }

    void initialize(int32_t max_val, int32_t rush_max) {
        if (max_val > 0 && rush_max > 0) {
            gauge_max_default = max_val;
            gauge_max_rush = rush_max;
            change_max(max_val);
        }
    }

    int32_t current_lot_type() const {
        constexpr std::array<int32_t, 4> kLotByRush{0, 4, 3, 2};
        uint32_t rc = static_cast<uint32_t>(rush_combo);
        return rc < 4 ? kLotByRush[rc] : 1;
    }

    void add_score(int32_t r) {
        constexpr std::array<int32_t, 3> kBonusByResult{5, 10, 10};
        uint32_t i = static_cast<uint32_t>(r - 1);
        if (i < 3u)
            total_bonus_point = wrap_add(total_bonus_point, kBonusByResult[i]);
        if (r >= 0 && r <= 3)
            results[r]++;
        if (static_cast<uint32_t>(r) < 3u) {
            rush_combo = 0;
            change_max(gauge_max_default);
        } else if (r == 3) {
            rush_combo = wrap_add(rush_combo, 1);
            change_max(gauge_max_rush);
        }
    }
};

struct FactorStorage {
    std::vector<std::pair<int32_t, float>> lot;
    std::vector<std::pair<int32_t, float>> gauge;
    std::unordered_map<int32_t, float> lot_ids;
    std::unordered_map<int32_t, float> gauge_ids;
    int32_t current = 0;

    static float at(const std::vector<std::pair<int32_t, float>> &cmds, int32_t t) {
        float s = 0.0f;
        for (const auto &[ct, d] : cmds)
            if (ct <= t)
                s = f32(s + d);
        return s;
    }

    int32_t add(bool is_gauge, int32_t t, float value) {
        current = wrap_add(current, 1);
        if (is_gauge) {
            gauge_ids[current] = value;
            gauge.emplace_back(t, value);
        } else {
            lot_ids[current] = value;
            lot.emplace_back(t, value);
        }
        return current;
    }

    void subtract(bool is_gauge, int32_t t, int32_t id) {
        auto &ids = is_gauge ? gauge_ids : lot_ids;
        auto it = ids.find(id);
        if (it == ids.end())
            return;
        float v = it->second;
        ids.erase(it);
        if (is_gauge)
            gauge.emplace_back(t, -v);
        else
            lot.emplace_back(t, -v);
    }
};

struct GkRange {
    int32_t start_ms = 0;
    int32_t end_ms = 0;
    int32_t mission = 0;
    std::unordered_set<int32_t> targets;
};

struct RangeState {
    int32_t state = kSWait;
    int32_t time_to = 0;
    int32_t combo = 0;
    int32_t max_combo = 0;
    int32_t just = 0;
    int32_t raw_just = 0;
    int32_t cum_base = 0;
    int32_t start_score = 0;
    int32_t end_score = 0;
    int32_t last_combo_ms = -1;
    int32_t last_just_ms = -1;
    LuckScore luck;
    float sw_elapsed = 0.0f;
    bool sw_stopped = true;

    int32_t score() const {
        return wrap_sub(end_score, start_score);
    }
};

struct LuckHandle {
    virtual ~LuckHandle() = default;
    virtual int32_t add(int32_t t, int32_t percent) = 0;
    virtual void disable(int32_t t, int32_t id) = 0;
};

struct GkEnv {
    LiveRandom &random;
    LuckHandle &handle;
};

using GkNote = std::array<int32_t, 4>; // [id, type, time_ms, judgement]

class Controller : public GekisouComboInfo {
public:
    std::vector<GkRange> ranges;
    std::vector<RangeState> states;
    LotteryMachine machine;
    std::vector<int32_t> lot_results;
    std::vector<int32_t> state_updates;
    int32_t current_playing_index = -1;
    bool weighted = false;

    Controller(const std::vector<std::array<int32_t, 3>> &range_defs,
               const std::vector<std::pair<int32_t, int32_t>> &notes, const GekisouTables &t,
               int32_t rush_percent, int32_t last_note_delay_ms)
        : machine(t), rush_percent_(rush_percent), last_note_delay_ms_(last_note_delay_ms),
          luck_gauge_max_(t.gauge_max) {
        for (const auto &[s, e, m] : range_defs) {
            GkRange r{s, e, m, {}};
            for (const auto &[id, tm] : notes) {
                if (s <= tm && tm <= e)
                    r.targets.insert(id);
            }
            ranges.push_back(std::move(r));
            RangeState st;
            if (m == kMLuck)
                st.luck.initialize(t.gauge_max, t.gauge_max_rush);
            states.push_back(std::move(st));
        }
        size_t n = ranges.size();
        history_.resize(n);
        snapshot_.resize(n);
        resume_.resize(n);
        pending_.resize(n);
    }

    int32_t playing_range_index() const {
        return playing_.empty() ? -1 : playing_.back();
    }

    void add_just_count(int32_t time_ms, int32_t count) {
        add_just_stack_.push_back({time_ms, f32(count), playing_range_index()});
        flag();
    }

    void add_gekisou_combo(int32_t time_ms, int32_t count) {
        add_combo_stack_.push_back({time_ms, f32(count), playing_range_index()});
        flag();
    }

    void add_luck_point(int32_t p) {
        int32_t i = playing_range_index();
        if (i >= 0)
            states[i].luck.total_bonus_point = wrap_add(states[i].luck.total_bonus_point, p);
    }

    void add_luck_gauge_percent(int32_t g) {
        int32_t i = playing_range_index();
        if (i >= 0)
            states[i].luck.add_gauge(g);
    }

    int32_t current_gauge_max() const {
        int32_t i = playing_range_index();
        return i >= 0 ? states[i].luck.gauge_max : luck_gauge_max_;
    }

    int32_t add_combo_bonus(int32_t t, float bonus) {
        int32_t i = next_id();
        combo_bonus_ids_[i] = bonus;
        cb_stack_.push_back({t, bonus, wrap_sub(seq_, 1)});
        flag();
        return i;
    }

    void subtract_combo_bonus(int32_t t, int32_t id) {
        auto it = combo_bonus_ids_.find(id);
        if (it == combo_bonus_ids_.end())
            return;
        float v = it->second;
        combo_bonus_ids_.erase(it);
        cb_stack_.push_back({t, -v, wrap_sub(seq_, 1)});
        flag();
    }

    int32_t add_just_bonus(int32_t t, float bonus) {
        int32_t i = next_id();
        just_bonus_ids_[i] = bonus;
        jb_stack_.push_back({t, bonus, wrap_sub(seq_, 1)});
        flag();
        return i;
    }

    void subtract_just_bonus(int32_t t, int32_t id) {
        auto it = just_bonus_ids_.find(id);
        if (it == just_bonus_ids_.end())
            return;
        float v = it->second;
        just_bonus_ids_.erase(it);
        jb_stack_.push_back({t, -v, wrap_sub(seq_, 1)});
        flag();
    }

    int32_t add_cumulative_rule(int32_t unit, int32_t effect, int32_t max_cum, int32_t max_eff) {
        int32_t i = next_id();
        rules_[i] = {playing_range_index(), unit, effect, max_cum, max_eff};
        flag();
        return i;
    }

    void remove_cumulative_rule(int32_t id) {
        if (rules_.erase(id))
            flag();
    }

    int32_t enable_combo_protect(int32_t t, int32_t limit, int32_t mask) {
        int32_t i = next_id();
        if (limit < 1) {
            pr_stack_.push_back({t, 1, i, mask & 0xff});
        } else {
            limited_ids_.insert(i);
            pr_stack_.push_back({t, wrap_add(limit, 1), i, mask & 0xff});
        }
        flag();
        return i;
    }

    void disable_combo_protect(int32_t t, int32_t id) {
        int32_t d = limited_ids_.erase(id) ? -2 : -1;
        pr_stack_.push_back({t, d, id, 0});
        flag();
    }

    int32_t add_lot_probability_up(int32_t t, float value) {
        return storage_.add(false, t, value);
    }
    int32_t add_gauge_up(int32_t t, float value) {
        return storage_.add(true, t, value);
    }
    void subtract_lot_probability_up(int32_t t, int32_t id) {
        storage_.subtract(false, t, id);
    }
    void subtract_gauge_up(int32_t t, int32_t id) {
        storage_.subtract(true, t, id);
    }

    float gauge_speed_at(int32_t t) const {
        return FactorStorage::at(storage_.gauge, t);
    }
    int32_t lot_buff_at(int32_t t) const {
        return floor_to_i32(f32(FactorStorage::at(storage_.lot, t) * 100.0f));
    }

    void update(int32_t t, const std::vector<GkNote> &judged,
                const std::vector<std::pair<int32_t, int32_t>> &fever_updates,
                int32_t current_score, GkEnv &env) {
        for (size_t idx = 0; idx < states.size(); ++idx) {
            if (states[idx].state > kSStandby && !pending_[idx].empty()) {
                auto pend = std::move(pending_[idx]);
                pending_[idx].clear();
                for (const auto &p : pend)
                    update_luck(static_cast<int32_t>(idx), p[0], p[1], p[3], env);
            }
        }
        for (size_t idx = 0; idx < states.size(); ++idx) {
            for (const auto &jn : judged) {
                if (ranges[idx].targets.count(jn[0]))
                    judge(static_cast<int32_t>(idx), jn[1], jn[2], jn[3], env);
            }
        }
        if (needs_recalc_) {
            recalculate();
            needs_recalc_ = false;
        }
        for (const auto &[idx, fs] : fever_updates) {
            if (fs == kFeverEnd)
                states.at(idx).end_score = current_score;
        }
        pending_lots(t, env);
    }

    void before_update(float dt, int32_t t,
                       const std::vector<std::pair<int32_t, int32_t>> &fever_updates,
                       int32_t current_score, GkEnv &env) {
        state_updates.clear();
        lot_result_ = -1;
        lot_results.clear();
        current_playing_index = -1;
        for (size_t i = 0; i < states.size(); ++i)
            states[i].time_to = wrap_sub(ranges[i].start_ms, t);
        std::vector<int32_t> remove;
        for (int32_t idx : playing_) {
            auto &rs = states[idx];
            int32_t st = rs.state;
            if (st < kSEnd) {
                if (st == kSStart) {
                    rs.state = kSPlaying;
                    state_update(idx);
                    current_playing_index = idx;
                } else if (st == kSPlaying) {
                    current_playing_index = idx;
                }
            } else if (st == kSEnd) {
                current_playing_index = idx;
                if (!rs.sw_stopped)
                    rs.sw_elapsed = f32(rs.sw_elapsed + dt);
                if (last_note_delay_ms_ <= elapsed_ms(rs)) {
                    rs.sw_elapsed = 0.0f;
                    rs.sw_stopped = false;
                    rs.state = kSDelay;
                    state_update(idx);
                }
            } else if (st == kSDelay) {
                if (!rs.sw_stopped)
                    rs.sw_elapsed = f32(rs.sw_elapsed + dt);
                if (500 <= elapsed_ms(rs)) {
                    rs.state = kSComplete;
                    rs.sw_stopped = true;
                    state_update(idx);
                }
            } else if (st == kSComplete) {
                rs.state = kSFinish;
                state_update(idx);
                remove.push_back(idx);
                disable_rush(t, env);
            }
        }
        for (int32_t idx : remove) {
            auto it = std::find(playing_.begin(), playing_.end(), idx);
            if (it != playing_.end())
                playing_.erase(it);
        }
        for (const auto &[idx, fs] : fever_updates) {
            auto &rs = states.at(idx);
            if (fs == kFeverEnd) {
                rs.state = kSEnd;
                rs.sw_elapsed = 0.0f;
                rs.sw_stopped = false;
                state_update(idx);
            } else if (fs == kFeverFever) {
                rs.state = kSStart;
                rs.start_score = current_score;
                playing_.push_back(idx);
                state_update(idx);
                current_playing_index = idx;
            }
        }
        for (size_t idx = 0; idx < states.size(); ++idx) {
            if (states[idx].state == kSWait && states[idx].time_to < 4001) {
                states[idx].state = kSStandby;
                state_update(static_cast<int32_t>(idx));
            }
        }
    }

    std::optional<int32_t> gekisou_combo(int32_t time_ms) const override {
        for (size_t i = 0; i < ranges.size(); ++i) {
            const auto &r = ranges[i];
            if (r.mission == kMCombo && r.start_ms <= time_ms && time_ms <= r.end_ms)
                return timing_combo_in_range(static_cast<int32_t>(i), time_ms);
        }
        return std::nullopt;
    }

private:
    struct Resume {
        size_t processed = 0;
        int32_t combo = 0;
        int32_t max_combo = 0;
        int32_t just = 0;
        int32_t raw_just = 0;
        int32_t cum_base = 0;
        float combo_bonus = 1.0f;
        float just_bonus = 1.0f;
        size_t i_cb = 0;
        size_t i_jb = 0;
        size_t i_add_just = 0;
        size_t i_add_combo = 0;
    };

    struct Rule {
        int32_t range_index = -1;
        int32_t unit = 0;
        int32_t effect = 0;
        int32_t max_cum = 0;
        int32_t max_eff = 0;
    };

    struct FloatStackEntry {
        int32_t time_ms = 0;
        float val = 0.0f;
        int32_t aux = 0;
    };

    static int64_t elapsed_ms(const RangeState &rs) {
        float v = f32(rs.sw_elapsed * 1000.0f);
        if (v == std::numeric_limits<float>::infinity())
            return -9007199254740991LL;
        return static_cast<int64_t>(std::trunc(v));
    }

    int32_t next_id() {
        handle_id_ = wrap_add(handle_id_, 1);
        return handle_id_;
    }

    void flag() {
        needs_recalc_ = true;
        full_recalc_ = true;
    }

    void state_update(int32_t idx) {
        if (std::find(state_updates.begin(), state_updates.end(), idx) == state_updates.end())
            state_updates.push_back(idx);
    }

    void apply_fixed_count(int32_t idx, float delta, int32_t range, bool just) {
        if (range != -1 && range != idx)
            return;
        int32_t n = static_cast<int32_t>(std::trunc(delta));
        auto &rs = states[idx];
        if (just) {
            rs.just = wrap_add(rs.just, n);
            rs.cum_base = wrap_add(rs.cum_base, n);
        } else {
            rs.combo = wrap_add(rs.combo, n);
            rs.max_combo = std::max(rs.max_combo, rs.combo);
        }
    }

    int32_t cumulative_just_bonus(int32_t range_index, int32_t x) const {
        int32_t total = 0;
        for (const auto &[_, r] : rules_) {
            if (r.range_index != range_index || r.unit <= 0)
                continue;
            int32_t steps = x / r.unit;
            if (r.max_cum > 0 && steps > r.max_cum)
                steps = r.max_cum;
            int32_t v = wrap_mul(steps, r.effect);
            if (r.max_eff > 0 && r.max_eff <= v)
                v = r.max_eff;
            total = wrap_add(total, v);
        }
        return total;
    }

    void recalculate() {
        for (size_t idx = 0; idx < states.size(); ++idx) {
            if (states[idx].state >= kSDelay || history_[idx].empty())
                continue;
            auto &rr = resume_[idx];
            auto &rs = states[idx];
            const auto &h = history_[idx];
            float cb = 1.0f, jb = 1.0f;
            size_t i_cb = 0, i_jb = 0, i_pr = 0, k = 0, i_add_just = 0, i_add_combo = 0;
            if (!full_recalc_ && pr_stack_.empty() && 1 <= rr.processed &&
                rr.processed <= h.size()) {
                rs.combo = rr.combo;
                rs.max_combo = rr.max_combo;
                rs.just = rr.just;
                rs.raw_just = rr.raw_just;
                rs.cum_base = rr.cum_base;
                cb = rr.combo_bonus;
                jb = rr.just_bonus;
                i_cb = rr.i_cb;
                i_jb = rr.i_jb;
                k = rr.processed;
                i_add_just = rr.i_add_just;
                i_add_combo = rr.i_add_combo;
            } else {
                rs.combo = rs.max_combo = rs.just = rs.raw_just = rs.cum_base = 0;
                snapshot_[idx].clear();
                unlimited_.clear();
                limited_.clear();
            }
            while (k < h.size()) {
                auto [et, j, es] = h[k];
                while (i_cb < cb_stack_.size()) {
                    const auto &c = cb_stack_[i_cb];
                    if (et <= c.time_ms && (et < c.time_ms || es < c.aux))
                        break;
                    cb = f32(cb + c.val);
                    ++i_cb;
                }
                while (i_jb < jb_stack_.size()) {
                    const auto &c = jb_stack_[i_jb];
                    if (et <= c.time_ms && (et < c.time_ms || es < c.aux))
                        break;
                    jb = f32(jb + c.val);
                    ++i_jb;
                }
                while (i_pr < pr_stack_.size() && et >= pr_stack_[i_pr][0]) {
                    auto [_, d, pid, mask] = pr_stack_[i_pr];
                    if (d < 2) {
                        if (d == 1) {
                            unlimited_.emplace_back(pid, mask);
                        } else if (d < -1) {
                            auto it = std::find_if(limited_.begin(), limited_.end(),
                                                   [&](const auto &x) { return x[0] == pid; });
                            if (it != limited_.end())
                                limited_.erase(it);
                        } else {
                            auto it = std::find_if(unlimited_.begin(), unlimited_.end(),
                                                   [&](const auto &x) { return x.first == pid; });
                            if (it != unlimited_.end())
                                unlimited_.erase(it);
                        }
                    } else {
                        limited_.push_back({pid, wrap_sub(d, 1), mask});
                    }
                    ++i_pr;
                }
                while (i_add_just < add_just_stack_.size() &&
                       add_just_stack_[i_add_just].time_ms <= et) {
                    const auto &c = add_just_stack_[i_add_just];
                    apply_fixed_count(static_cast<int32_t>(idx), c.val, c.aux, true);
                    ++i_add_just;
                }
                while (i_add_combo < add_combo_stack_.size() &&
                       add_combo_stack_[i_add_combo].time_ms <= et) {
                    const auto &c = add_combo_stack_[i_add_combo];
                    apply_fixed_count(static_cast<int32_t>(idx), c.val, c.aux, false);
                    ++i_add_combo;
                }
                entry(static_cast<int32_t>(idx), j, floor_to_i32(cb), floor_to_i32(jb));
                snapshot_[idx].push_back(rs.combo);
                ++k;
            }
            resume_[idx] = {h.size(), rs.combo, rs.max_combo, rs.just, rs.raw_just, rs.cum_base,
                            cb,       jb,       i_cb,         i_jb,    i_add_just,  i_add_combo};
            for (size_t i = i_add_just; i < add_just_stack_.size(); ++i) {
                const auto &c = add_just_stack_[i];
                apply_fixed_count(static_cast<int32_t>(idx), c.val, c.aux, true);
            }
            for (size_t i = i_add_combo; i < add_combo_stack_.size(); ++i) {
                const auto &c = add_combo_stack_[i];
                apply_fixed_count(static_cast<int32_t>(idx), c.val, c.aux, false);
            }
        }
        full_recalc_ = false;
    }

    void entry(int32_t idx, int32_t j, int32_t i_combo, int32_t i_just) {
        auto &rs = states[idx];
        if (static_cast<uint32_t>(j - 3) < 0xfffffffeu) {
            if (static_cast<uint32_t>(j - 7) > 0xfffffffbu) {
                rs.combo = wrap_add(rs.combo, i_combo);
                rs.max_combo = std::max(rs.max_combo, rs.combo);
            } else {
                return;
            }
        } else {
            int32_t bit = 1 << (j & 31);
            auto eff = [](int32_t m) { return m != 0 ? m : 6; };
            bool prot = false;
            for (const auto &[_, m] : unlimited_) {
                if ((eff(m) & bit) != 0) {
                    prot = true;
                    break;
                }
            }
            for (ptrdiff_t i = static_cast<ptrdiff_t>(limited_.size()) - 1; i >= 0; --i) {
                auto [pid, rem, m] = limited_[static_cast<size_t>(i)];
                if ((eff(m) & bit) != 0) {
                    int32_t left = wrap_sub(rem, 1);
                    if (left < 1)
                        limited_.erase(limited_.begin() + i);
                    else
                        limited_[static_cast<size_t>(i)] = {pid, left, m};
                    prot = true;
                }
            }
            if (!prot) {
                rs.max_combo = std::max(rs.max_combo, rs.combo);
                rs.combo = 0;
            }
        }
        if (j == 6) {
            rs.raw_just = wrap_add(rs.raw_just, 1);
            rs.cum_base = wrap_add(rs.cum_base, i_just);
            int32_t c = cumulative_just_bonus(idx, rs.cum_base);
            rs.just = wrap_add(wrap_add(c, i_just), rs.just);
        }
    }

    int32_t timing_combo_in_range(int32_t idx, int32_t t) const {
        const auto &h = history_[idx];
        const auto &sn = snapshot_[idx];
        size_t n = std::min(h.size(), sn.size());
        if (n == 0)
            return 0;
        int32_t score_frame = t > 0 ? static_cast<int32_t>(std::ceil(f32(f32(t) / 40.0f))) : 0;
        int32_t prev_end = wrap_sub(wrap_mul(score_frame, 40), 40);
        if (prev_end < h[0][0])
            return 0;
        int32_t hi = static_cast<int32_t>(n) - 1;
        if (hi < 1)
            return sn[0];
        int32_t lo = 0;
        for (;;) {
            int32_t mid = lo + sdiv(hi - lo + 1, 2);
            if (h[static_cast<size_t>(mid)][0] <= prev_end)
                lo = mid;
            else
                hi = mid - 1;
            if (lo >= hi)
                return sn[static_cast<size_t>(lo)];
        }
    }

    void update_luck(int32_t idx, int32_t nt, int32_t tn, int32_t j, GkEnv &env) {
        if (weighted)
            return;
        int32_t buff = floor_to_i32(f32(FactorStorage::at(storage_.lot, tn) * 100.0f));
        machine.set_buff(buff);
        float gup = FactorStorage::at(storage_.gauge, tn);
        int32_t base = machine.base_point(nt, j, env.random);
        int32_t add = floor_to_i32(f32(f32(gup + 1.0f) * f32(base)));
        states[idx].luck.add_gauge(add);
        consume_lot(idx, tn, env);
    }

    void disable_rush(int32_t t, GkEnv &env) {
        if (rush_id_ != 0) {
            env.handle.disable(t, rush_id_);
            rush_id_ = 0;
        }
    }

    void consume_lot(int32_t idx, int32_t t, GkEnv &env) {
        auto &st = states[idx];
        if (st.state > kSEnd || st.luck.lot_count < 1)
            return;
        st.luck.lot_count--;
        if (st.luck.next == -1)
            st.luck.next = machine.luck_bonus(0, t, env.random);
        int32_t next = st.luck.next;
        int32_t rush = st.luck.rush_combo;
        if (rush == 0) {
            if (next == 3)
                rush_id_ = env.handle.add(t, rush_percent_);
            else
                disable_rush(t, env);
        } else if (next != 3) {
            disable_rush(t, env);
        }
        st.luck.add_score(next);
        if (next != -1) {
            lot_result_ = next;
            lot_results.push_back(next);
        }
        st.luck.next = machine.luck_bonus(st.luck.current_lot_type(), t, env.random);
    }

    void pending_lots(int32_t t, GkEnv &env) {
        if (weighted)
            return;
        auto pl = playing_;
        for (int32_t idx : pl) {
            auto &rs = states[idx];
            if (ranges[idx].mission == kMLuck && rs.state == kSPlaying && rs.luck.lot_count != 0 &&
                lot_result_ == -1) {
                machine.set_buff(floor_to_i32(f32(FactorStorage::at(storage_.lot, t) * 100.0f)));
                consume_lot(idx, t, env);
            }
        }
    }

    void judge(int32_t idx, int32_t nt, int32_t tn, int32_t j, GkEnv &env) {
        if (j == 7 || j == 0)
            return;
        history_[idx].push_back({tn, j, seq_});
        seq_ = wrap_add(seq_, 1);
        needs_recalc_ = true;
        if (static_cast<uint32_t>(j - 3) < 4u)
            states[idx].last_combo_ms = tn;
        if (j == 6)
            states[idx].last_just_ms = tn;
        if (ranges[idx].mission == kMLuck) {
            if (states[idx].state < kSStart)
                pending_[idx].push_back({nt, tn, 0, j});
            else
                update_luck(idx, nt, tn, j, env);
        }
    }

    FactorStorage storage_;
    int32_t rush_percent_;
    int32_t last_note_delay_ms_;
    int32_t luck_gauge_max_;
    int32_t rush_id_ = 0;
    std::vector<int32_t> playing_;
    std::unordered_map<int32_t, float> combo_bonus_ids_;
    std::unordered_map<int32_t, float> just_bonus_ids_;
    std::unordered_map<int32_t, Rule> rules_;
    int32_t handle_id_ = 0;
    bool needs_recalc_ = false;
    bool full_recalc_ = false;
    int32_t seq_ = 0;
    std::vector<std::vector<std::array<int32_t, 3>>> history_;
    std::vector<std::vector<int32_t>> snapshot_;
    std::vector<Resume> resume_;
    std::vector<FloatStackEntry> cb_stack_;
    std::vector<FloatStackEntry> jb_stack_;
    std::vector<FloatStackEntry> add_just_stack_;
    std::vector<FloatStackEntry> add_combo_stack_;
    std::vector<std::array<int32_t, 4>> pr_stack_;
    std::vector<std::pair<int32_t, int32_t>> unlimited_;
    std::vector<std::array<int32_t, 3>> limited_;
    std::unordered_set<int32_t> limited_ids_;
    std::vector<std::vector<std::array<int32_t, 4>>> pending_;
    int32_t lot_result_ = -1;
};

class FeverUpdater {
public:
    explicit FeverUpdater(std::vector<std::pair<int32_t, int32_t>> fevers)
        : fevers_(std::move(fevers)), state_(fevers_.size(), kFeverWait) {}

    const std::vector<std::pair<int32_t, int32_t>> &update(int32_t t) {
        out_.clear();
        for (size_t i = 0; i < fevers_.size(); ++i) {
            auto [s, e] = fevers_[i];
            int32_t st = state_[i];
            int32_t next = (st == kFeverWait && t >= s)
                               ? kFeverFever
                               : ((st == kFeverFever && t >= e) ? kFeverEnd : 0);
            if (!next)
                continue;
            state_[i] = next;
            out_.emplace_back(static_cast<int32_t>(i), next);
        }
        return out_;
    }

private:
    std::vector<std::pair<int32_t, int32_t>> fevers_;
    std::vector<int32_t> state_;
    std::vector<std::pair<int32_t, int32_t>> out_;
};

// 技能条件检查器、累积条件与状态机
struct GkView {
    Controller *ctrl = nullptr;
    std::vector<int32_t> prev_lots;
    int32_t prev_lot_ms = 0;
};

struct CheckCtx {
    std::optional<int32_t> prev_confirmed_rank;
    LifeController *life = nullptr;
    LiveRandom *random = nullptr;
    int32_t frame_time = 0;
    int32_t current_combo = 0;
    const std::vector<std::array<int32_t, 3>> *judged = nullptr;
    const std::vector<std::pair<int32_t, int32_t>> *events = nullptr;
    std::optional<GkView> gk;
    bool record = false;
};

class Checker {
public:
    virtual ~Checker() = default;
    virtual std::pair<bool, int32_t> check(CheckCtx &ctx) = 0;
    virtual std::optional<int32_t> override_time() const {
        return std::nullopt;
    }
    virtual void reset(CheckCtx &) {}
    virtual bool count_resettable() const {
        return false;
    }
    virtual void reset_count(CheckCtx &) {}
    virtual std::optional<int32_t> idle_member() const {
        return std::nullopt;
    }
    virtual double chance(CheckCtx &ctx) {
        return check(ctx).first ? 1.0 : 0.0;
    }
};

class FixedChecker : public Checker {
public:
    explicit FixedChecker(bool ok) : ok_(ok) {}
    std::pair<bool, int32_t> check(CheckCtx &) override {
        return {ok_, ok_ ? 1 : 0};
    }
    std::optional<int32_t> idle_member() const override {
        return ok_ ? std::nullopt : std::optional<int32_t>(-1);
    }

private:
    bool ok_;
};

class AndChecker : public Checker {
public:
    explicit AndChecker(std::vector<std::shared_ptr<Checker>> items) : items_(std::move(items)) {
        for (const auto &it : items_)
            resettable_.push_back(it->count_resettable());
    }
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        int32_t hit = 0;
        for (size_t i = 0; i < items_.size(); ++i) {
            auto [ok, h] = items_[i]->check(ctx);
            hit = std::max(hit, h);
            if (!ok) {
                if (!resettable_[i]) {
                    for (size_t k = 0; k < items_.size(); ++k)
                        if (resettable_[k])
                            items_[k]->reset_count(ctx);
                }
                return {false, 0};
            }
        }
        return {true, hit};
    }
    void reset(CheckCtx &ctx) override {
        for (auto &it : items_)
            it->reset(ctx);
    }
    double chance(CheckCtx &ctx) override {
        double p = 1.0;
        for (auto &it : items_)
            p *= it->chance(ctx);
        return p;
    }

private:
    std::vector<std::shared_ptr<Checker>> items_;
    std::vector<bool> resettable_;
};

class OrChecker : public Checker {
public:
    explicit OrChecker(std::vector<std::shared_ptr<Checker>> items) : items_(std::move(items)) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        int32_t hit = 0;
        for (auto &c : items_) {
            auto [ok, h] = c->check(ctx);
            hit = std::max(hit, h);
            if (ok)
                return {true, hit};
        }
        return {false, 0};
    }
    void reset(CheckCtx &ctx) override {
        for (auto &it : items_)
            it->reset(ctx);
    }
    double chance(CheckCtx &ctx) override {
        double prod = 1.0;
        for (auto &it : items_)
            prod *= (1.0 - it->chance(ctx));
        return 1.0 - prod;
    }

private:
    std::vector<std::shared_ptr<Checker>> items_;
};

class NotChecker : public Checker {
public:
    explicit NotChecker(std::shared_ptr<Checker> inner) : inner_(std::move(inner)) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        auto [ok, h] = inner_->check(ctx);
        return {!ok, h};
    }
    std::optional<int32_t> override_time() const override {
        return inner_->override_time();
    }
    void reset(CheckCtx &ctx) override {
        inner_->reset(ctx);
    }
    bool count_resettable() const override {
        return true;
    }
    void reset_count(CheckCtx &ctx) override {
        if (inner_->count_resettable())
            inner_->reset_count(ctx);
    }
    double chance(CheckCtx &ctx) override {
        return 1.0 - inner_->chance(ctx);
    }

private:
    std::shared_ptr<Checker> inner_;
};

class LifeChecker : public Checker {
public:
    LifeChecker(int32_t type, int32_t value) : type_(type), value_(value) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        int32_t life = ctx.life->life_at(ctx.frame_time);
        bool ok = (type_ == 2000)   ? (value_ < life)
                  : (type_ == 2001) ? (value_ <= life)
                  : (type_ == 2002) ? (life < value_)
                                    : (life <= value_);
        return {ok, ok ? 1 : 0};
    }

private:
    int32_t type_;
    int32_t value_;
};

class LifeChangedChecker : public Checker {
public:
    explicit LifeChangedChecker(int32_t previous) : previous_(previous) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        int32_t life = ctx.life->life_at(ctx.frame_time);
        bool ok = (previous_ != life);
        previous_ = life;
        return {ok, 0};
    }

private:
    int32_t previous_;
};

class LifePercentChecker : public Checker {
public:
    explicit LifePercentChecker(int32_t threshold) : threshold_(threshold) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        int32_t life = ctx.life->life_at(ctx.frame_time);
        bool ok = (previous_ > threshold_ && life <= threshold_);
        previous_ = life;
        return {ok, ok ? 1 : 0};
    }

private:
    int32_t threshold_;
    int32_t previous_ = 0;
};

class LifeDeltaChecker : public Checker {
public:
    LifeDeltaChecker(int32_t threshold, bool increase, int32_t previous)
        : threshold_(threshold), increase_(increase), previous_(previous) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        int32_t life = ctx.life->life_at(ctx.frame_time);
        int32_t change = increase_ ? wrap_sub(life, previous_) : wrap_sub(previous_, life);
        previous_ = life;
        if (change > 0)
            count_ = wrap_add(count_, change);
        int32_t hits = (threshold_ > 0 && count_ >= threshold_) ? (count_ / threshold_) : 0;
        if (hits > 0)
            count_ %= threshold_;
        return {hits > 0, hits};
    }
    void reset(CheckCtx &ctx) override {
        count_ = 0;
        previous_ = ctx.life->life_at(ctx.frame_time);
    }
    bool count_resettable() const override {
        return true;
    }
    void reset_count(CheckCtx &ctx) override {
        reset(ctx);
    }

private:
    int32_t threshold_;
    bool increase_;
    int32_t previous_;
    int32_t count_ = 0;
};

class LiveComboMultipleChecker : public Checker {
public:
    explicit LiveComboMultipleChecker(int32_t threshold) : threshold_(threshold) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        int32_t combo = ctx.current_combo;
        if (combo < 1 || threshold_ < 1) {
            previous_ = 0;
            return {false, 0};
        }
        if (combo <= previous_) {
            previous_ = combo;
            return {false, 0};
        }
        int32_t hit = (combo / threshold_) - (previous_ / threshold_);
        previous_ = combo;
        return {hit > 0, hit};
    }
    void reset(CheckCtx &) override {
        previous_ = 0;
    }

private:
    int32_t threshold_;
    int32_t previous_ = 0;
};

class LiveComboAtLeastChecker : public Checker {
public:
    explicit LiveComboAtLeastChecker(int32_t threshold) : threshold_(threshold) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        bool hit = threshold_ > 0 && ctx.current_combo >= threshold_;
        return {hit, hit ? 1 : 0};
    }

private:
    int32_t threshold_;
};

class ElapsedTimeChecker : public Checker {
public:
    ElapsedTimeChecker(int32_t period, int32_t previous) : period_(period), previous_(previous) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        elapsed_ = wrap_add(elapsed_, wrap_sub(ctx.frame_time, previous_));
        previous_ = ctx.frame_time;
        bool hit = elapsed_ >= period_;
        if (hit)
            elapsed_ = wrap_sub(elapsed_, period_);
        return {hit, hit ? 1 : 0};
    }
    void reset(CheckCtx &ctx) override {
        elapsed_ = 0;
        previous_ = ctx.frame_time;
    }
    bool count_resettable() const override {
        return true;
    }
    void reset_count(CheckCtx &ctx) override {
        reset(ctx);
    }

private:
    int32_t period_;
    int32_t previous_;
    int32_t elapsed_ = 0;
};

class SnapGekisouStartChecker : public Checker {
public:
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        if (ctx.gk) {
            for (int32_t idx : ctx.gk->ctrl->state_updates) {
                if (ctx.gk->ctrl->states[idx].state == kSStart && idx != previous_) {
                    previous_ = idx;
                    return {true, 1};
                }
            }
        }
        return {false, 0};
    }
    void reset(CheckCtx &) override {
        previous_ = -1;
    }

private:
    int32_t previous_ = -1;
};

class SameMemberLiveSkillChecker : public Checker {
public:
    explicit SameMemberLiveSkillChecker(int32_t member) : member_(member) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        bool ok = false;
        if (ctx.events) {
            for (const auto &[idx, _] : *ctx.events) {
                if (idx == member_) {
                    ok = true;
                    break;
                }
            }
        }
        return {ok, ok ? 1 : 0};
    }
    std::optional<int32_t> idle_member() const override {
        return member_;
    }

private:
    int32_t member_;
};

class ProbabilityChecker : public Checker {
public:
    explicit ProbabilityChecker(float rate) : rate_(rate) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        if (ctx.record)
            return {true, 0};
        return {ctx.random->value(LiveRandom::kSkill) < rate_, 0};
    }
    double chance(CheckCtx &) override {
        return static_cast<double>(rate_);
    }

private:
    float rate_;
};

class NoteJudgementMatchChecker : public Checker {
public:
    NoteJudgementMatchChecker(int32_t kind, std::vector<int32_t> targets)
        : kind_(kind), targets_(std::move(targets)) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        override_.reset();
        int32_t hit = 0;
        if (ctx.judged) {
            for (const auto &[nid, j, tn] : *ctx.judged) {
                if (kind_ == 1020 && (j == -1 || j == 7))
                    continue;
                for (int32_t target : targets_) {
                    bool matches = (kind_ == 1000)   ? (target == j)
                                   : (kind_ == 1010) ? (j >= target)
                                                     : (j <= target);
                    if (matches) {
                        ++hit;
                        if (nid >= 0)
                            override_ = tn;
                        else
                            override_.reset();
                        break;
                    }
                }
            }
        }
        return {hit > 0, hit};
    }
    std::optional<int32_t> override_time() const override {
        return override_;
    }
    void reset(CheckCtx &) override {
        override_.reset();
    }

private:
    int32_t kind_;
    std::vector<int32_t> targets_;
    std::optional<int32_t> override_;
};

class NoteJudgementCountChecker : public Checker {
public:
    NoteJudgementCountChecker(int32_t n, bool consecutive, std::vector<int32_t> targets)
        : n_(n), consecutive_(consecutive), targets_(std::move(targets)) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        override_.reset();
        int32_t trig = -1;
        int32_t trig_ms = 0;
        bool ok = false;
        int32_t hit = 0;
        if (ctx.judged) {
            for (const auto &[nid, j, tn] : *ctx.judged) {
                for (int32_t tj : targets_) {
                    if (tj == j) {
                        count_ = wrap_add(count_, 1);
                        if (count_ >= n_) {
                            ok = true;
                            ++hit;
                            count_ = 0;
                            trig = nid;
                            trig_ms = tn;
                        }
                    } else if (consecutive_) {
                        count_ = 0;
                    }
                }
            }
        }
        if (ok && trig >= 0)
            override_ = trig_ms;
        return {ok, hit};
    }
    std::optional<int32_t> override_time() const override {
        return override_;
    }
    void reset(CheckCtx &) override {
        count_ = 0;
    }
    bool count_resettable() const override {
        return true;
    }
    void reset_count(CheckCtx &ctx) override {
        reset(ctx);
    }

private:
    int32_t n_;
    bool consecutive_;
    std::vector<int32_t> targets_;
    int32_t count_ = 0;
    std::optional<int32_t> override_;
};

class LuckLotResultChecker : public Checker {
public:
    explicit LuckLotResultChecker(int32_t target) : target_(target) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        override_.reset();
        int32_t hit = 0;
        if (ctx.gk) {
            for (int32_t r : ctx.gk->prev_lots)
                if (r == target_)
                    ++hit;
            if (hit > 0) {
                override_ = ctx.gk->prev_lot_ms;
                return {true, hit};
            }
        }
        return {false, 0};
    }
    std::optional<int32_t> override_time() const override {
        return override_;
    }
    void reset(CheckCtx &) override {
        override_.reset();
    }

private:
    int32_t target_;
    std::optional<int32_t> override_;
};

class ComboAtLeastChecker : public Checker {
public:
    explicit ComboAtLeastChecker(int32_t threshold) : threshold_(threshold) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        override_.reset();
        if (!ctx.gk)
            return {false, 0};
        auto *c = ctx.gk->ctrl;
        int32_t idx = c->current_playing_index;
        if (idx < 0 || threshold_ <= 0)
            return {false, 0};
        const auto &rs = c->states[idx];
        if (rs.combo < threshold_)
            return {false, 0};
        if (rs.last_combo_ms >= 0)
            override_ = rs.last_combo_ms;
        return {true, 1};
    }
    std::optional<int32_t> override_time() const override {
        return override_;
    }
    void reset(CheckCtx &) override {
        override_.reset();
    }

private:
    int32_t threshold_;
    std::optional<int32_t> override_;
};

class GkIntervalChecker : public Checker {
public:
    GkIntervalChecker(int32_t kind, int32_t n) : kind_(kind), n_(n) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        override_.reset();
        int32_t idx = ctx.gk ? ctx.gk->ctrl->current_playing_index : -1;
        const RangeState *rs = (ctx.gk && idx >= 0) ? &ctx.gk->ctrl->states[idx] : nullptr;
        int32_t hits = 0;
        if (n_ > 0 && rs) {
            int32_t current = (kind_ == 7001)   ? rs->combo
                              : (kind_ == 7002) ? rs->just
                                                : rs->raw_just;
            if (current <= 0) {
                previous_ = 0;
            } else {
                int32_t old = previous_;
                previous_ = current;
                if (current > old)
                    hits = std::max(0, (current / n_) - (old / n_));
            }
        }
        if (hits > 0 && rs) {
            int32_t t = (kind_ == 7001) ? rs->last_combo_ms : rs->last_just_ms;
            if (t >= 0)
                override_ = t;
        }
        return {hits > 0, hits};
    }
    std::optional<int32_t> override_time() const override {
        return override_;
    }
    void reset(CheckCtx &) override {
        previous_ = 0;
        override_.reset();
    }

private:
    int32_t kind_;
    int32_t n_;
    int32_t previous_ = 0;
    std::optional<int32_t> override_;
};

class JustAtLeastChecker : public Checker {
public:
    explicit JustAtLeastChecker(int32_t threshold) : threshold_(threshold) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        override_.reset();
        int32_t idx = ctx.gk ? ctx.gk->ctrl->current_playing_index : -1;
        const RangeState *rs = (ctx.gk && idx >= 0) ? &ctx.gk->ctrl->states[idx] : nullptr;
        bool hit = threshold_ > 0 && rs != nullptr && rs->just >= threshold_;
        if (hit && rs->last_just_ms >= 0)
            override_ = rs->last_just_ms;
        return {hit, hit ? 1 : 0};
    }
    std::optional<int32_t> override_time() const override {
        return override_;
    }
    void reset(CheckCtx &) override {
        override_.reset();
    }

private:
    int32_t threshold_;
    std::optional<int32_t> override_;
};

class GkJustEdgeChecker : public Checker {
public:
    GkJustEdgeChecker(bool raw, int32_t threshold) : raw_(raw), threshold_(threshold) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        override_.reset();
        int32_t idx = ctx.gk ? ctx.gk->ctrl->current_playing_index : -1;
        const RangeState *rs = (ctx.gk && idx >= 0) ? &ctx.gk->ctrl->states[idx] : nullptr;
        if (threshold_ <= 0)
            return {false, 0};
        if (!rs) {
            previous_ = 0;
            last_range_ = -1;
            return {false, 0};
        }
        if (idx != last_range_) {
            previous_ = 0;
            last_range_ = idx;
        }
        int32_t count = raw_ ? rs->raw_just : rs->just;
        bool hit = previous_ < threshold_ && count >= threshold_;
        previous_ = count;
        if (hit && rs->last_just_ms >= 0)
            override_ = rs->last_just_ms;
        return {hit, hit ? 1 : 0};
    }
    std::optional<int32_t> override_time() const override {
        return override_;
    }
    void reset(CheckCtx &) override {
        previous_ = 0;
        last_range_ = -1;
        override_.reset();
    }

private:
    bool raw_;
    int32_t threshold_;
    int32_t previous_ = 0;
    int32_t last_range_ = -1;
    std::optional<int32_t> override_;
};

class GkOnceChecker : public Checker {
public:
    explicit GkOnceChecker(std::function<bool(CheckCtx &)> eligible)
        : eligible_(std::move(eligible)) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        bool hit = !triggered_ && eligible_(ctx);
        if (hit)
            triggered_ = true;
        return {hit, hit ? 1 : 0};
    }
    void reset(CheckCtx &) override {
        triggered_ = false;
    }

private:
    std::function<bool(CheckCtx &)> eligible_;
    bool triggered_ = false;
};

inline bool mission_ok(const std::vector<int32_t> &missions, int32_t mission) {
    if (missions.empty())
        return true;
    for (int32_t m : missions)
        if (m == kMAll || m == mission)
            return true;
    return false;
}

class RangeStartChecker : public Checker {
public:
    explicit RangeStartChecker(std::vector<int32_t> missions) : missions_(std::move(missions)) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        override_.reset();
        if (!ctx.gk)
            return {false, 0};
        auto *c = ctx.gk->ctrl;
        for (int32_t idx : c->state_updates) {
            int32_t s = c->states[idx].state;
            if (s == kSFinish) {
                triggered_.erase(std::remove(triggered_.begin(), triggered_.end(), idx),
                                 triggered_.end());
            } else if (s == kSStart) {
                if (!mission_ok(missions_, c->ranges[idx].mission))
                    continue;
                if (std::find(triggered_.begin(), triggered_.end(), idx) == triggered_.end()) {
                    triggered_.push_back(idx);
                    override_ = c->ranges[idx].start_ms;
                    return {true, 1};
                }
            }
        }
        return {false, 0};
    }
    std::optional<int32_t> override_time() const override {
        return override_;
    }
    void reset(CheckCtx &) override {
        triggered_.clear();
        override_.reset();
    }

private:
    std::vector<int32_t> missions_;
    std::vector<int32_t> triggered_;
    std::optional<int32_t> override_;
};

class RangeCompleteChecker : public Checker {
public:
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        if (!ctx.gk)
            return {false, 0};
        auto *c = ctx.gk->ctrl;
        for (int32_t i : c->state_updates) {
            if (c->states[i].state == kSComplete)
                return {true, 1};
        }
        return {false, 0};
    }
};

class RangePlayingChecker : public Checker {
public:
    explicit RangePlayingChecker(std::vector<int32_t> missions) : missions_(std::move(missions)) {}
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        override_.reset();
        if (!ctx.gk)
            return {false, 0};
        auto *c = ctx.gk->ctrl;
        int32_t first = -1;
        for (int32_t idx : c->state_updates) {
            int32_t s = c->states[idx].state;
            if (s == kSStart || s == kSPlaying) {
                bool exists = false;
                for (const auto &a : active_)
                    if (a.first == idx)
                        exists = true;
                if (!exists) {
                    active_.emplace_back(idx, c->ranges[idx].mission);
                    if (first < 0)
                        first = c->ranges[idx].start_ms;
                }
            } else if (s == kSComplete || s == kSFinish) {
                active_.erase(std::remove_if(active_.begin(), active_.end(),
                                             [&](const auto &a) { return a.first == idx; }),
                              active_.end());
            }
        }
        if (active_.empty())
            return {false, 0};
        if (!missions_.empty() &&
            std::find(missions_.begin(), missions_.end(), kMAll) == missions_.end()) {
            bool any_m = false;
            for (const auto &a : active_) {
                if (std::find(missions_.begin(), missions_.end(), a.second) != missions_.end()) {
                    any_m = true;
                    break;
                }
            }
            if (!any_m)
                return {false, 0};
        }
        if (first >= 0)
            override_ = first;
        return {true, 1};
    }
    std::optional<int32_t> override_time() const override {
        return override_;
    }
    void reset(CheckCtx &) override {
        active_.clear();
        override_.reset();
    }

private:
    std::vector<int32_t> missions_;
    std::vector<std::pair<int32_t, int32_t>> active_;
    std::optional<int32_t> override_;
};

class LuckRushPlayingChecker : public Checker {
public:
    std::pair<bool, int32_t> check(CheckCtx &ctx) override {
        if (ctx.record || !ctx.gk)
            return {false, 0};
        auto *c = ctx.gk->ctrl;
        int32_t idx = c->current_playing_index;
        if (idx >= 0 && c->ranges[idx].mission == kMLuck)
            rush_ = (c->states[idx].luck.rush_combo != 0);
        for (int32_t i : c->state_updates) {
            int32_t s = c->states[i].state;
            if (c->ranges[i].mission == kMLuck && (s == kSComplete || s == kSFinish))
                rush_ = false;
        }
        return {rush_, rush_ ? 1 : 0};
    }
    void reset(CheckCtx &) override {
        rush_ = false;
    }

private:
    bool rush_ = false;
};

inline double chance_of(const std::shared_ptr<Checker> &checker, CheckCtx &ctx) {
    return checker ? checker->chance(ctx) : 1.0;
}

class Cumulative {
public:
    virtual ~Cumulative() = default;
    virtual int32_t init_count() const {
        return 0;
    }
    virtual int32_t unit() const {
        return 0;
    }
    virtual int32_t max_val() const {
        return 0;
    }
    virtual int32_t update_count(CheckCtx &ctx) = 0;
    virtual void reset() {}
};

class CumulativeFixed : public Cumulative {
public:
    explicit CumulativeFixed(int32_t value) : value_(value) {}
    int32_t init_count() const override {
        return value_;
    }
    int32_t update_count(CheckCtx &) override {
        return value_;
    }

private:
    int32_t value_;
};

class ComboPerN : public Cumulative {
public:
    ComboPerN(int32_t n, int32_t maximum, bool just) : n_(n), maximum_(maximum), just_(just) {}
    int32_t unit() const override {
        return n_;
    }
    int32_t max_val() const override {
        return maximum_;
    }
    int32_t update_count(CheckCtx &ctx) override {
        if (!ctx.gk)
            return 0;
        int32_t idx = ctx.gk->ctrl->current_playing_index;
        int32_t v = 0;
        if (idx >= 0) {
            const auto &st = ctx.gk->ctrl->states[idx];
            v = floor_to_i32(f32(f32(just_ ? st.just : st.combo) / f32(n_)));
        }
        return std::min(v, maximum_);
    }

private:
    int32_t n_;
    int32_t maximum_;
    bool just_;
};

class LifePerN : public Cumulative {
public:
    LifePerN(int32_t init, int32_t n, int32_t maximum, bool limit)
        : init_(init), n_(n), maximum_(maximum), limit_(limit) {}
    int32_t init_count() const override {
        return init_;
    }
    int32_t update_count(CheckCtx &ctx) override {
        int32_t life = limit_ ? ctx.life->maximum() : ctx.life->life_at(ctx.frame_time);
        return std::min(floor_to_i32(f32(f32(life) / f32(n_))), maximum_);
    }

private:
    int32_t init_;
    int32_t n_;
    int32_t maximum_;
    bool limit_;
};

class JudgementPerN : public Cumulative {
public:
    JudgementPerN(int32_t n, std::vector<int32_t> targets, int32_t maximum, int32_t kind)
        : n_(n), targets_(std::move(targets)), maximum_(maximum), kind_(kind) {}
    int32_t unit() const override {
        return n_;
    }
    int32_t max_val() const override {
        return maximum_;
    }
    int32_t update_count(CheckCtx &ctx) override {
        if (ctx.judged) {
            for (const auto &jn : *ctx.judged) {
                int32_t j = jn[1];
                bool match = false;
                for (int32_t t : targets_) {
                    if ((kind_ == 1001) ? (j >= t) : (kind_ == 1002) ? (j <= t) : (j == t)) {
                        match = true;
                        break;
                    }
                }
                if (match)
                    count_ = wrap_add(count_, 1);
            }
        }
        return std::min(floor_to_i32(f32(f32(count_) / f32(n_))), maximum_);
    }
    void reset() override {
        count_ = 0;
    }

private:
    int32_t n_;
    std::vector<int32_t> targets_;
    int32_t maximum_;
    int32_t kind_;
    int32_t count_ = 0;
};

class ElapsedCount : public Cumulative {
public:
    ElapsedCount(int32_t period, int32_t previous, int32_t maximum)
        : period_(period), previous_(previous), maximum_(maximum) {}
    int32_t update_count(CheckCtx &ctx) override {
        elapsed_ = wrap_add(elapsed_, wrap_sub(ctx.frame_time, previous_));
        previous_ = ctx.frame_time;
        if (elapsed_ >= period_) {
            elapsed_ = wrap_sub(elapsed_, period_);
            count_ = wrap_add(count_, 1);
        }
        return std::min(count_, maximum_);
    }
    void reset() override {
        elapsed_ = 0;
        count_ = 0;
    }

private:
    int32_t period_;
    int32_t previous_;
    int32_t maximum_;
    int32_t elapsed_ = 0;
    int32_t count_ = 0;
};

class Factory {
public:
    Factory(const EngineMaster &master, const std::vector<Performer> &deck, int32_t initial_life,
            int32_t skill_target_music_type, int32_t initial_time_ms = 0)
        : master_(master), deck_(deck), initial_life_(initial_life),
          skill_target_music_type_(skill_target_music_type), initial_time_ms_(initial_time_ms) {}

    std::shared_ptr<Checker> group(int32_t gid, size_t k) const {
        if (!gid)
            return nullptr;
        auto it = master_.condition_sets.find(gid);
        if (it == master_.condition_sets.end())
            return nullptr;
        std::vector<std::shared_ptr<Checker>> ors;
        for (const auto &set : it->second) {
            std::vector<std::shared_ptr<Checker>> items;
            for (int32_t cid : set) {
                auto ch = one(cid, k);
                if (ch)
                    items.push_back(std::move(ch));
            }
            if (items.size() == 1)
                ors.push_back(std::move(items[0]));
            else if (items.size() > 1)
                ors.push_back(std::make_shared<AndChecker>(std::move(items)));
        }
        if (ors.empty())
            return nullptr;
        if (ors.size() == 1)
            return ors[0];
        return std::make_shared<OrChecker>(std::move(ors));
    }

    std::shared_ptr<Cumulative> cumulative(int32_t cid, size_t k) const {
        if (!cid)
            return nullptr;
        const auto &c = master_.cumulative.at(cid);
        int32_t max_c = c.cap < 1 ? kI32Max : c.cap;
        int32_t n = c.values.empty() ? 0 : c.values[0];
        switch (c.type) {
        case 7000:
        case 7001:
            return std::make_shared<ComboPerN>(n, max_c, c.type == 7000);
        case 2000:
        case 2001: {
            int32_t init = (c.type == 2000) ? floor_to_i32(f32(f32(initial_life_) / f32(n))) : 0;
            return std::make_shared<LifePerN>(init, n, max_c, c.type == 2001);
        }
        case 1000:
        case 1001:
        case 1002: {
            std::vector<int32_t> targets;
            for (int32_t id : c.target_ids) {
                int32_t j = master_.targets.at(id).judgement;
                if (j != -1)
                    targets.push_back(j);
            }
            return std::make_shared<JudgementPerN>(n, std::move(targets), max_c, c.type);
        }
        case 3000:
        case 3001:
        case 3002:
        case 3003:
        case 3004:
        case 3005: {
            int32_t count = 0;
            if (c.type <= 3001) {
                for (size_t idx = 0; idx < deck_.size(); ++idx) {
                    if (c.type == 3001 && idx == k)
                        continue;
                    for (int32_t tid : c.target_ids) {
                        if (performer_matches(deck_[idx], master_.targets.at(tid))) {
                            ++count;
                            break;
                        }
                    }
                }
            } else if (c.type <= 3003) {
                int32_t band = deck_.at(k).band_id;
                if (c.type == 3002) {
                    for (const auto &p : deck_)
                        if (p.band_id == band)
                            ++count;
                } else {
                    std::unordered_set<int32_t> bs;
                    for (const auto &p : deck_)
                        if (p.band_id != band)
                            bs.insert(p.band_id);
                    count = static_cast<int32_t>(bs.size());
                }
            } else {
                std::unordered_set<int32_t> vals;
                for (const auto &p : deck_)
                    vals.insert(c.type == 3004 ? p.band_id : p.card_type);
                count = static_cast<int32_t>(vals.size());
            }
            return std::make_shared<CumulativeFixed>(std::min(count, max_c));
        }
        case 6000:
            return std::make_shared<ElapsedCount>(wrap_mul(n, 1000), initial_time_ms_, max_c);
        default:
            throw std::runtime_error("未知的累积条件类型: " + std::to_string(c.type));
        }
    }

private:
    std::shared_ptr<Checker> one(int32_t cid, size_t k) const {
        const auto &c = master_.conditions.at(cid);
        int32_t v0 = c.values.empty() ? 0 : c.values[0];
        auto missions = [&]() {
            std::vector<int32_t> ms;
            for (int32_t id : c.target_ids) {
                const auto &t = master_.targets.at(id);
                if (t.skill_target_type == 5 && t.gekisou_mission_type != 0)
                    ms.push_back(t.gekisou_mission_type);
            }
            return ms;
        };
        std::shared_ptr<Checker> ch;
        switch (c.type) {
        case 0:
            return nullptr;
        case 2000:
        case 2001:
        case 2002:
        case 2003:
            ch = std::make_shared<LifeChecker>(c.type, v0);
            break;
        case 2004:
            ch = std::make_shared<LifeChangedChecker>(initial_life_);
            break;
        case 4007:
            ch = std::make_shared<LifePercentChecker>(
                ceil_to_i32(f32(f32(f32(v0) / 10.0f) * f32(initial_life_))));
            break;
        case 4008:
        case 4009:
            ch = std::make_shared<LifeDeltaChecker>(v0, c.type == 4008, initial_life_);
            break;
        case 3000:
        case 3001: {
            size_t count = 0;
            for (const auto &member : deck_) {
                for (int32_t id : c.target_ids) {
                    if (performer_matches(member, master_.targets.at(id))) {
                        ++count;
                        break;
                    }
                }
            }
            ch = std::make_shared<FixedChecker>(c.type == 3000 ? (count > 0)
                                                               : (count == deck_.size()));
            break;
        }
        case 4001:
            ch = std::make_shared<LiveComboMultipleChecker>(v0);
            break;
        case 4002:
            ch = std::make_shared<LiveComboAtLeastChecker>(v0);
            break;
        case 4000:
            ch = std::make_shared<ElapsedTimeChecker>(wrap_mul(v0, 1000), initial_time_ms_);
            break;
        case 4010:
        case 5020:
            ch = std::make_shared<SameMemberLiveSkillChecker>(static_cast<int32_t>(k));
            break;
        case 5021:
            ch = std::make_shared<SnapGekisouStartChecker>();
            break;
        case 4012: {
            bool ok = false;
            for (int32_t id : c.target_ids) {
                if (master_.targets.at(id).live_music_type == skill_target_music_type_) {
                    ok = true;
                    break;
                }
            }
            ch = std::make_shared<FixedChecker>(ok);
            break;
        }
        case 4011:
            ch = std::make_shared<ProbabilityChecker>(f32(f32(v0) / 100.0f));
            break;
        case 5000: {
            bool ok = false;
            if (k < deck_.size()) {
                for (int32_t id : c.target_ids) {
                    if (performer_matches(deck_[k], master_.targets.at(id))) {
                        ok = true;
                        break;
                    }
                }
            }
            ch = std::make_shared<FixedChecker>(ok);
            break;
        }
        case 8000:
            ch = std::make_shared<FixedChecker>(false);
            break;
        case 1000:
        case 1010:
        case 1020:
        case 1030:
        case 1040: {
            std::vector<int32_t> judgements;
            for (int32_t id : c.target_ids) {
                int32_t j = master_.targets.at(id).judgement;
                if (j != -1)
                    judgements.push_back(j);
            }
            if (c.type == 1030 || c.type == 1040)
                ch = std::make_shared<NoteJudgementCountChecker>(v0, c.type == 1040,
                                                                 std::move(judgements));
            else
                ch = std::make_shared<NoteJudgementMatchChecker>(c.type, std::move(judgements));
            break;
        }
        case 7000:
            ch = std::make_shared<LuckLotResultChecker>(v0);
            break;
        case 7001:
        case 7002:
        case 7004:
            ch = std::make_shared<GkIntervalChecker>(c.type, v0);
            break;
        case 7003:
            ch = std::make_shared<JustAtLeastChecker>(v0);
            break;
        case 7006:
        case 7007:
            ch = std::make_shared<GkJustEdgeChecker>(c.type == 7007, v0);
            break;
        case 7011:
            ch = std::make_shared<GkOnceChecker>([](CheckCtx &ctx) { return ctx.gk.has_value(); });
            break;
        case 7012: {
            int32_t threshold = c.values.empty() ? 1 : v0;
            ch = std::make_shared<GkOnceChecker>([threshold](CheckCtx &ctx) {
                return ctx.prev_confirmed_rank.has_value() && *ctx.prev_confirmed_rank > 0 &&
                       *ctx.prev_confirmed_rank <= threshold;
            });
            break;
        }
        case 7005:
            ch = std::make_shared<ComboAtLeastChecker>(v0);
            break;
        case 7010:
            ch = std::make_shared<RangeStartChecker>(missions());
            break;
        case 7013:
            ch = std::make_shared<RangeCompleteChecker>();
            break;
        case 7020:
            ch = std::make_shared<RangePlayingChecker>(missions());
            break;
        case 7021:
            ch = std::make_shared<LuckRushPlayingChecker>();
            break;
        default:
            throw std::runtime_error("不支持的技能条件类型: " + std::to_string(c.type));
        }
        return c.positive ? ch : std::make_shared<NotChecker>(std::move(ch));
    }

    const EngineMaster &master_;
    const std::vector<Performer> &deck_;
    int32_t initial_life_;
    int32_t skill_target_music_type_;
    int32_t initial_time_ms_;
};

struct EffectState {
    int32_t state = kStay;
    int32_t execute_ms = -1;
    int32_t finish_ms = -1;
    float extended_ms = 0.0f;
    int32_t cumulative_count = 0;
    int32_t cumulative_unit = 0;
    int32_t cumulative_max = 0;
};

struct FrameInput {
    int32_t time_ms = 0;
    int32_t music_length_ms = 0;
    bool finished = false;
};

struct Trigger {
    bool hit = false;
    int32_t time_ms = 0;
};

bool effect_update(EffectState &st, float act, const FrameInput &inp, const Trigger &trigger,
                   Checker *condition, Checker *release, bool finish_frame, CheckCtx &ctx) {
    switch (st.state) {
    case kStay: {
        bool ok = condition ? condition->check(ctx).first : true;
        if (ok && trigger.hit) {
            st.state = kExecuteFrame;
            st.execute_ms = trigger.time_ms;
        }
        return ok && trigger.hit;
    }
    case kExecuteFrame: {
        int32_t t = inp.time_ms;
        if (finish_frame) {
            st.state = kEndFrame;
            st.finish_ms = t;
        } else if (release) {
            st.state = kExecuting;
        } else {
            float dur = f32(f32(act * 1000.0f) + st.extended_ms);
            float elapsed = f32(wrap_sub(t, st.execute_ms));
            if (dur > elapsed) {
                st.state = kExecuting;
            } else {
                st.state = kEndFrame;
                st.finish_ms = t;
            }
        }
        return true;
    }
    case kExecuting: {
        bool released = release ? release->check(ctx).first : false;
        float dur = f32(f32(act * 1000.0f) + st.extended_ms);
        float elapsed = f32(wrap_sub(inp.time_ms, st.execute_ms));
        bool by_time = (dur < elapsed && act > 0.0f);
        if (!(released || finish_frame || by_time))
            return false;
        st.state = kEndFrame;
        int32_t finish =
            (!released && !finish_frame) ? wrap_add(st.execute_ms, ceil_to_i32(dur)) : inp.time_ms;
        if (inp.music_length_ms > 0 && inp.music_length_ms <= finish)
            finish = inp.music_length_ms;
        st.finish_ms = finish;
        return true;
    }
    case kEndFrame:
        st.state = kStay;
        return true;
    default:
        return false;
    }
}

inline bool has_activation_time(float act) {
    if (std::isnan(act) || act <= 0.0f)
        return false;
    float big = f32(kI32Max);
    float m = std::max(std::abs(act), big);
    float tol = std::max(f32(m * f32(1e-6)), f32(1.401298464324817e-45 * 8.0));
    return tol <= std::abs(f32(big - act));
}

struct CondEffect {
    int32_t effect_id = 0;
    int32_t trigger_type = 0;
    float act = 0.0f;
    int32_t phase = 1;
    std::shared_ptr<Checker> trigger;
    std::shared_ptr<Checker> condition;
    int32_t execute_limit = 0;
    std::shared_ptr<Checker> reset;
    size_t row = 0;
    std::function<std::shared_ptr<Cumulative>()> cumulative;
};

struct EffectUpdater {
    EffectState state;
    size_t effect = 0;
    int32_t index = 0;
    std::shared_ptr<Checker> release;
    int32_t phase = 1;
    std::shared_ptr<Cumulative> cumulative;

    EffectUpdater(size_t ef, int32_t idx, std::shared_ptr<Checker> rel, int32_t ph,
                  std::shared_ptr<Cumulative> cum)
        : effect(ef), index(idx), release(std::move(rel)), phase(ph), cumulative(std::move(cum)) {
        if (cumulative) {
            state.cumulative_count = cumulative->init_count();
            state.cumulative_unit = cumulative->unit();
            state.cumulative_max = cumulative->max_val();
        }
    }

    bool update(float act, const FrameInput &inp, const Trigger &trigger, bool finish_frame,
                CheckCtx &ctx) {
        int32_t s0 = state.state;
        bool r = effect_update(state, act, inp, trigger, nullptr, release.get(), finish_frame, ctx);
        if (cumulative) {
            int32_t s1 = state.state;
            if ((s0 == kStay && s1 == kExecuteFrame) || s0 == kExecuteFrame ||
                (s0 == kExecuting && s1 == kExecuting))
                state.cumulative_count = cumulative->update_count(ctx);
            else if (s0 == kEndFrame)
                cumulative->reset();
        }
        return r;
    }
};

inline bool mission_gate_open(std::optional<int32_t> gate, const CheckCtx &ctx) {
    if (!gate.has_value() || *gate == kMAll)
        return true;
    if (!ctx.gk)
        return false;
    const auto *c = ctx.gk->ctrl;
    for (int32_t i : c->state_updates) {
        if (c->ranges[i].mission == *gate)
            return true;
    }
    int32_t i = c->current_playing_index;
    return i >= 0 && c->ranges[i].mission == *gate;
}

class ConditionSkillUpdater {
public:
    std::vector<CondEffect> effects;
    std::vector<EffectUpdater> updaters;
    std::optional<int32_t> gate;

    ConditionSkillUpdater(std::vector<CondEffect> effects_in,
                          const std::function<std::shared_ptr<Checker>(size_t)> &release_fn,
                          std::optional<int32_t> gate_in)
        : effects(std::move(effects_in)), gate(gate_in) {
        sorted_.resize(effects.size());
        std::iota(sorted_.begin(), sorted_.end(), 0);
        std::sort(sorted_.begin(), sorted_.end(),
                  [&](size_t a, size_t b) { return effects[a].effect_id < effects[b].effect_id; });
        stacks_.resize(effects.size());
        sustained_.resize(effects.size());
        for (size_t e = 0; e < effects.size(); ++e) {
            const auto &ef = effects[e];
            if (ef.trigger_type != kOneShot && ef.trigger_type != kSustained)
                continue;
            std::vector<size_t> pool;
            for (int32_t idx = 0; idx < 5; ++idx) {
                pool.push_back(updaters.size());
                updaters.emplace_back(e, idx, release_fn(e), ef.phase,
                                      ef.cumulative ? ef.cumulative() : nullptr);
            }
            if (ef.trigger_type == kOneShot) {
                stacks_[e] = std::move(pool);
            } else {
                Sustained s;
                s.present = true;
                s.queue = std::move(pool);
                s.timed = has_activation_time(ef.act);
                sustained_[e] = std::move(s);
            }
        }
        cache_.resize(effects.size());
        if (!effects.empty()) {
            std::vector<int32_t> idle;
            bool ok = true;
            for (const auto &ef : effects) {
                auto m = ef.trigger ? ef.trigger->idle_member() : std::nullopt;
                if (ef.trigger_type != kOneShot || ef.reset != nullptr || !m.has_value()) {
                    ok = false;
                    break;
                }
                if (*m >= 0 && std::find(idle.begin(), idle.end(), *m) == idle.end())
                    idle.push_back(*m);
            }
            if (ok)
                idle_members_ = std::move(idle);
        }
    }

    void begin_frame() {
        trigger_checked_ = false;
        quiet_frame_ = false;
    }

    std::vector<size_t> update(int32_t phase, const FrameInput &inp, CheckCtx &ctx) {
        if (!trigger_checked_ && executing_.empty()) {
            bool idle_skip = false;
            if (idle_members_.has_value()) {
                bool any_ev = false;
                if (ctx.events) {
                    for (const auto &[m, _] : *ctx.events) {
                        if (std::find(idle_members_->begin(), idle_members_->end(), m) !=
                            idle_members_->end()) {
                            any_ev = true;
                            break;
                        }
                    }
                }
                if (!any_ev)
                    idle_skip = true;
            }
            if (idle_skip || inp.finished || !mission_gate_open(gate, ctx)) {
                trigger_checked_ = true;
                quiet_frame_ = true;
            }
        }
        if (quiet_frame_)
            return {};
        std::vector<size_t> updated;
        std::vector<size_t> done;
        auto exec_copy = executing_;
        for (size_t u : exec_copy) {
            if (phase < 1 || updaters[u].phase == phase) {
                update_one(u, inp, {false, 0}, false, ctx);
                if (updaters[u].state.state == kStay)
                    done.push_back(u);
                else
                    updated.push_back(u);
            }
        }
        if (!trigger_checked_) {
            trigger_checked_ = true;
            std::fill(cache_.begin(), cache_.end(), std::nullopt);
            if (inp.finished || !mission_gate_open(gate, ctx)) {
                finish(done);
                return updated;
            }
            for (const auto &ef : effects) {
                if (ef.reset && ef.reset->check(ctx).first)
                    execute_count_.erase(ef.effect_id);
            }
            for (size_t e : sorted_) {
                auto &c = effects[e].trigger;
                if (!c) {
                    cache_[e] = Trigger{false, 0};
                } else {
                    bool hit = c->check(ctx).first;
                    cache_[e] = Trigger{hit, c->override_time().value_or(inp.time_ms)};
                }
            }
        }
        for (size_t e : sorted_) {
            if (!cache_[e].has_value())
                continue;
            Trigger tr = *cache_[e];
            auto &ef = effects[e];
            if (ef.trigger_type == kSustained) {
                if (phase < 1 || ef.phase == phase)
                    update_sustained(e, inp, tr, ctx, updated);
                continue;
            }
            if (!tr.hit)
                continue;
            auto &stack = stacks_[e];
            if (stack.empty())
                continue;
            size_t top = stack.back();
            if (phase >= 1 && updaters[top].phase != phase)
                continue;
            auto ec_it = execute_count_.find(ef.effect_id);
            if (ef.execute_limit > 0 && ec_it != execute_count_.end() &&
                ef.execute_limit <= ec_it->second)
                continue;
            if (ef.condition && !ef.condition->check(ctx).first)
                continue;
            size_t u = stack.back();
            stack.pop_back();
            update_one(u, inp, {true, tr.time_ms}, false, ctx);
            executing_.push_back(u);
            updated.push_back(u);
            if (ef.execute_limit > 0)
                execute_count_[ef.effect_id]++;
        }
        finish(done);
        return updated;
    }

private:
    struct Sustained {
        bool present = false;
        std::vector<size_t> queue;
        std::optional<size_t> current;
        bool enabled = false;
        bool timed = false;
        std::vector<size_t> executing;
    };

    bool update_one(size_t u, const FrameInput &inp, const Trigger &trigger, bool finish_frame,
                    CheckCtx &ctx) {
        auto &upd = updaters[u];
        return upd.update(effects[upd.effect].act, inp, trigger, finish_frame, ctx);
    }

    void finish(const std::vector<size_t> &done) {
        for (size_t u : done) {
            stacks_[updaters[u].effect].push_back(u);
            auto it = std::find(executing_.begin(), executing_.end(), u);
            if (it != executing_.end())
                executing_.erase(it);
        }
    }

    void update_sustained(size_t e, const FrameInput &inp, const Trigger &tr, CheckCtx &ctx,
                          std::vector<size_t> &updated) {
        auto &s = sustained_[e];
        if (s.timed) {
            timed_step(s, e, inp, tr, ctx, updated);
        } else {
            auto u = step(s, e, tr.hit ? tr.time_ms : inp.time_ms, inp, tr, ctx);
            if (u.has_value())
                updated.push_back(*u);
        }
    }

    void timed_step(Sustained &s, size_t e, const FrameInput &inp, const Trigger &tr, CheckCtx &ctx,
                    std::vector<size_t> &updated) {
        if (tr.hit) {
            auto &condition = effects[e].condition;
            if (!s.enabled) {
                if (condition)
                    condition->reset(ctx);
                s.enabled = true;
            }
            std::optional<size_t> fresh;
            if (!condition || condition->check(ctx).first) {
                if (condition)
                    condition->reset(ctx);
                size_t u = s.queue.front();
                s.queue.erase(s.queue.begin());
                s.executing.push_back(u);
                fresh = u;
            }
            std::vector<size_t> done;
            for (size_t u : s.executing) {
                bool is_fresh = (fresh.has_value() && *fresh == u);
                Trigger trigger{is_fresh, is_fresh ? tr.time_ms : inp.time_ms};
                if (update_one(u, inp, trigger, false, ctx)) {
                    updated.push_back(u);
                    if (updaters[u].state.state == kStay)
                        done.push_back(u);
                }
            }
            for (size_t u : done) {
                s.queue.push_back(u);
                s.executing.erase(std::remove(s.executing.begin(), s.executing.end(), u),
                                  s.executing.end());
            }
        } else {
            for (size_t u : s.executing) {
                update_one(u, inp, {false, 0}, true, ctx);
                updated.push_back(u);
                s.queue.push_back(u);
            }
            s.executing.clear();
            s.enabled = false;
        }
    }

    std::optional<size_t> step(Sustained &s, size_t e, int32_t execute_ms, const FrameInput &inp,
                               const Trigger &tr, CheckCtx &ctx) {
        auto cur = s.current;
        if (cur.has_value() && updaters[*cur].state.state == kEndFrame) {
            update_one(*cur, inp, {false, 0}, false, ctx);
            s.queue.push_back(*cur);
            s.current.reset();
            cur.reset();
        }
        if (cur.has_value() && updaters[*cur].state.state == kExecuteFrame)
            updaters[*cur].state.state = kExecuting;
        auto &condition = effects[e].condition;
        if (tr.hit) {
            if (!s.enabled) {
                if (condition)
                    condition->reset(ctx);
                s.enabled = true;
            }
            if (condition && !condition->check(ctx).first)
                return std::nullopt;
            if (!cur.has_value()) {
                size_t c = s.queue.front();
                s.queue.erase(s.queue.begin());
                s.current = c;
                if (update_one(c, inp, {true, execute_ms}, false, ctx))
                    return c;
            }
        } else {
            if (cur.has_value()) {
                update_one(*cur, inp, {false, 0}, true, ctx);
                s.enabled = false;
                return *cur;
            }
            s.enabled = false;
        }
        return std::nullopt;
    }

    std::vector<size_t> sorted_;
    std::vector<std::vector<size_t>> stacks_;
    std::vector<Sustained> sustained_;
    std::vector<size_t> executing_;
    std::unordered_map<int32_t, int32_t> execute_count_;
    bool trigger_checked_ = false;
    std::vector<std::optional<Trigger>> cache_;
    std::optional<std::vector<int32_t>> idle_members_;
    bool quiet_frame_ = false;
};
// 整曲逐帧仿真模型 (LiveModel) 与幸运期望递推 (luck_curve)
inline int32_t convert_score_type(int32_t j) {
    constexpr std::array<int32_t, 9> kMap{0, 0, 6, 5, 4, 3, 2, 1, 0};
    size_t idx = static_cast<size_t>(j + 1);
    return idx < kMap.size() ? kMap[idx] : 0;
}

inline bool approximately(float a, float b) {
    float tol = std::max(f32(f32(1e-6) * std::max(std::abs(a), std::abs(b))),
                         f32(1.401298464324817e-45 * 8.0));
    return std::abs(f32(b - a)) < tol;
}

inline int32_t gekisou_mission_group(int32_t mission) {
    if (mission >= 1 && mission <= 3)
        return mission;
    if (mission == 4)
        return 1;
    throw std::runtime_error("未知的激奏任务类型: " + std::to_string(mission));
}

inline bool is_gekisou_applier_type(int32_t type) {
    switch (type) {
    case 11000:
    case 11001:
    case 11002:
    case 11003:
    case 11004:
    case 11005:
    case 12000:
    case 12002:
    case 12003:
    case 12004:
    case 13000:
    case 13002:
    case 13003:
    case 13004:
        return true;
    default:
        return false;
    }
}

class ScoreCtl : public LuckHandle {
public:
    explicit ScoreCtl(IncrementalScore &score) : score_(score) {}

    int32_t put(const FactorCommand &cmd) {
        counter_ = wrap_add(counter_, 1);
        cmds_[counter_] = cmd;
        score_.add_factor(cmd);
        return counter_;
    }

    int32_t add_note_score_up(int32_t owner, int32_t t, float factor) {
        return put({t, owner, note_factor_mill(factor), 0, 0, 0, 0});
    }

    void disable_note_score_up(int32_t t, int32_t id) {
        auto c = take(id);
        score_.add_factor({t, c.owner, wrap_sub(0, c.note), 0, 0, 0, 0});
    }

    int32_t add_combo_bonus(int32_t owner, int32_t t, float factor) {
        return put({t, owner, 0, judgement_factor_mill(factor), 0, 0, 0});
    }

    void disable_combo_bonus(int32_t t, int32_t id) {
        auto c = take(id);
        score_.add_factor({t, c.owner, 0, wrap_sub(0, c.combo), 0, 0, 0});
    }

    float combo_bonus_factor(int32_t id) const {
        auto it = cmds_.find(id);
        return (it != cmds_.end() && it->second.combo != 0) ? f32(f32(it->second.combo) / 100000.0f)
                                                            : 0.0f;
    }

    float score_up_factor(int32_t id) const {
        auto it = cmds_.find(id);
        return (it != cmds_.end() && it->second.note != 0) ? f32(f32(it->second.note) / 100000.0f)
                                                           : 0.0f;
    }

    int32_t add(int32_t t, int32_t percent) override {
        return put({t, -1, 0, 0, 0, 0, percent});
    }

    void disable(int32_t t, int32_t id) override {
        auto c = take(id);
        score_.add_factor({t, -1, 0, 0, 0, 0, wrap_sub(0, c.luck)});
    }

private:
    FactorCommand take(int32_t id) {
        auto it = cmds_.find(id);
        FactorCommand c = it->second;
        cmds_.erase(it);
        return c;
    }

    IncrementalScore &score_;
    int32_t counter_ = 0;
    std::unordered_map<int32_t, FactorCommand> cmds_;
};

struct GkAppliers {
    std::unordered_map<std::string, int32_t> ids;
    std::unordered_set<std::string> exhausted;
    std::unordered_map<std::string, int32_t> limit_finished;

    void add(const std::string &key, int32_t id) {
        ids[key] = id;
    }
    int32_t pop(const std::string &key) {
        auto it = ids.find(key);
        int32_t id = it->second;
        ids.erase(it);
        return id;
    }
    void limit(const std::string &key, EffectState &st) {
        auto it = limit_finished.find(key);
        if (it != limit_finished.end()) {
            int32_t t = it->second;
            limit_finished.erase(it);
            st.state = kEndFrame;
            st.finish_ms = t;
        }
    }
};

class LiveModel {
public:
    LifeController life;
    IncrementalScore score;

    LiveModel(const EngineMaster &master, const std::vector<Performer> &deck,
              const std::vector<LiveNote> &notes,
              const std::vector<std::pair<int32_t, int32_t>> &events, const LiveParams &params,
              const GekisouSetup *setup, int32_t seed = 0, LuckRecord *record = nullptr)
        : life(master.live.life_base, master.live.damage,
               notes.empty() ? 1000
                             : (std::max_element(notes.begin(), notes.end(),
                                                 [](const LiveNote &a, const LiveNote &b) {
                                                     return a.time_ms < b.time_ms;
                                                 })
                                    ->time_ms +
                                1000)),
          score(ScoreCalculator(master.live, params.power, params.level, params.converted_count,
                                params.assist),
                params.music_length_ms),
          events_(events), fired_(events.size(), false), random_(seed), conversion_([&]() {
              std::unordered_set<int32_t> nj;
              for (int32_t t : master.live.timing_types) {
                  if (!master.live.just_types.count(t))
                      nj.insert(t);
              }
              return nj;
          }()),
          params_(params), scorectl_(score), record_(record) {
        for (const auto &n : notes)
            notes_[n.id] = n;
        const auto &live = master.live;
        if (setup) {
            size_t n = std::min<size_t>(setup->fevers.size(), 3);
            std::vector<std::array<int32_t, 3>> range_defs;
            for (size_t i = 0; i < n; ++i) {
                range_defs.push_back(
                    {setup->fevers[i].first, setup->fevers[i].second, setup->missions.at(i)});
            }
            std::vector<std::pair<int32_t, int32_t>> note_pairs;
            note_pairs.reserve(notes.size());
            for (const auto &nt : notes)
                note_pairs.emplace_back(nt.id, nt.time_ms);
            gk_ = std::make_unique<GekisouLive>(
                Controller(range_defs, note_pairs, live.gekiso, live.gekiso.rush_percent,
                           live.after_ms),
                FeverUpdater(setup->fevers),
                ranking_factors(live.gekiso, mission_pattern(setup->missions[0], setup->missions[1],
                                                             setup->missions[2])));
            if (record_)
                gk_->ctrl.weighted = true;
        }

        ctx_.record = (record_ != nullptr);
        ctx_.life = &life;
        ctx_.random = &random_;
        ctx_.judged = &judged_;
        ctx_.events = &frame_events_;
        if (gk_)
            ctx_.gk = GkView{&gk_->ctrl, {}, 0};

        auto phase_of = [&](int32_t type) -> int32_t {
            auto it = live.phases.find(type);
            return it != live.phases.end() ? it->second : 1;
        };
        Factory factory(master, deck, live.life_base, params.skill_target_music_type);
        auto add_effect_row = [&](const SkillEffectRow &r) -> size_t {
            std::vector<int32_t> targets;
            for (int32_t t : r.target_ids)
                targets.push_back(master.targets.at(t).judgement);
            rows_.push_back(
                {r.id, r.type, r.value, r.max_value, r.limit_count, std::move(targets)});
            return rows_.size() - 1;
        };

        for (size_t k = 0; k < deck.size(); ++k) {
            const auto &p = deck[k];
            if (!p.live_skill.has_value())
                continue;
            auto [sid, lv] = *p.live_skill;
            std::vector<SkillEffectRow> rs;
            if (auto it = master.live_skills.find(sid); it != master.live_skills.end()) {
                for (const auto &r : it->second)
                    if (r.level == lv)
                        rs.push_back(r);
            }
            std::sort(rs.begin(), rs.end(),
                      [](const SkillEffectRow &a, const SkillEffectRow &b) { return a.id < b.id; });
            std::vector<size_t> row_indexes;
            for (const auto &r : rs)
                row_indexes.push_back(add_effect_row(r));
            int32_t key = sid * 1000 + 100 + static_cast<int32_t>(k);
            std::vector<size_t> pool;
            for (int32_t index = 0; index < 5; ++index) {
                pool.push_back(live_.size());
                std::vector<LiveEffect> effs;
                for (size_t i = 0; i < rs.size(); ++i) {
                    const auto &r = rs[i];
                    auto cum = factory.cumulative(r.cumulative_id, k);
                    EffectState st;
                    if (cum) {
                        st.cumulative_count = cum->init_count();
                        st.cumulative_unit = cum->unit();
                        st.cumulative_max = cum->max_val();
                    }
                    effs.push_back({row_indexes[i], f32(r.seconds), phase_of(r.type), st,
                                    factory.group(r.condition_group, k),
                                    factory.group(r.release_group, k), cum});
                }
                live_.push_back(
                    {key, static_cast<int32_t>(k), index, kStay, {false, 0}, std::move(effs)});
            }
            live_pools_.push_back({key, static_cast<int32_t>(k), std::move(pool)});
        }
        std::sort(live_pools_.begin(), live_pools_.end(),
                  [](const LivePool &a, const LivePool &b) { return a.key < b.key; });

        auto add_condition_skill = [&](std::vector<SkillEffectRow> rs, size_t k, int32_t skill_type,
                                       std::optional<int32_t> gate) {
            std::sort(rs.begin(), rs.end(),
                      [](const SkillEffectRow &a, const SkillEffectRow &b) { return a.id < b.id; });
            std::vector<CondEffect> effs;
            std::vector<int32_t> releases;
            for (const auto &r : rs) {
                int32_t cum_id = r.cumulative_id;
                effs.push_back({
                    r.id * 100 + skill_type * 10 + static_cast<int32_t>(k),
                    r.trigger_type,
                    f32(r.seconds),
                    phase_of(r.type),
                    factory.group(r.trigger_group, k),
                    factory.group(r.condition_group, k),
                    r.execute_limit,
                    r.execute_limit > 0 ? factory.group(r.reset_group, k) : nullptr,
                    add_effect_row(r),
                    cum_id ? std::function<std::shared_ptr<Cumulative>()>([&master, &deck, &live,
                                                                           &params, cum_id, k]() {
                        Factory f(master, deck, live.life_base, params.skill_target_music_type);
                        return f.cumulative(cum_id, k);
                    })
                           : nullptr,
                });
                releases.push_back(r.release_group);
            }
            ConditionSkillUpdater updater(
                std::move(effs), [&](size_t e) { return factory.group(releases[e], k); }, gate);
            cond_.push_back({static_cast<int32_t>(k), skill_type, std::move(updater)});
        };

        for (size_t k = 0; k < deck.size(); ++k) {
            for (const auto &[sid, lv] : deck[k].support_skills) {
                std::vector<SkillEffectRow> rs;
                if (auto it = master.support_skills.find(sid); it != master.support_skills.end()) {
                    for (const auto &r : it->second)
                        if (r.level == lv)
                            rs.push_back(r);
                }
                add_condition_skill(std::move(rs), k, 3, std::nullopt);
            }
        }
        if (gk_) {
            std::vector<std::optional<int32_t>> mission(deck.size());
            for (size_t k = 0; k < deck.size(); ++k) {
                const auto &p = deck[k];
                if (p.mission_only.has_value() && *p.mission_only != 0) {
                    mission[k] = *p.mission_only;
                } else if (p.gekisou_skill.has_value()) {
                    int32_t mt = master.gekiso_skills.at(p.gekisou_skill->first).mission_type;
                    gekisou_mission_group(mt);
                    mission[k] = mt;
                }
            }
            for (int32_t m = 1; m <= 3; ++m) {
                for (size_t k = 0; k < deck.size(); ++k) {
                    const auto &p = deck[k];
                    if (!p.gekisou_skill.has_value() || !mission[k].has_value() ||
                        gekisou_mission_group(*mission[k]) != m)
                        continue;
                    auto [sid, lv] = *p.gekisou_skill;
                    std::vector<SkillEffectRow> rs;
                    for (const auto &r : master.gekiso_skills.at(sid).effects)
                        if (r.level == lv)
                            rs.push_back(r);
                    add_condition_skill(std::move(rs), k, 4, *mission[k]);
                }
            }
            for (size_t k = 0; k < deck.size(); ++k) {
                if (!mission[k].has_value())
                    continue;
                for (const auto &[sid, lv] : deck[k].gekisou_support_skills) {
                    const auto &row = master.gekiso_support_skills.at(sid);
                    std::vector<SkillEffectRow> rs;
                    for (const auto &r : row.effects)
                        if (r.level == lv)
                            rs.push_back(r);
                    add_condition_skill(std::move(rs), k, 5, row.mission_type);
                }
            }
        }
    }

    void frame(int32_t t, const std::vector<JudgedNote> &judged, float dt) {
        frame_time_ = t;
        auto frame_rank = prev_confirmed_rank_;
        prev_confirmed_rank_.reset();
        if (gk_) {
            GkEnv env{random_, scorectl_};
            gk_->fever_updates = gk_->fever.update(t);
            gk_->ctrl.before_update(dt, t, gk_->fever_updates, score.score, env);
            if (record_) {
                std::vector<int32_t> sts;
                sts.reserve(gk_->ctrl.states.size());
                for (const auto &s : gk_->ctrl.states)
                    sts.push_back(s.state);
                record_->frames.push_back(
                    {std::move(sts), gk_->ctrl.state_updates, gk_->ctrl.current_playing_index});
            }
        }
        frame_events_.clear();
        if (fired_count_ < events_.size()) {
            for (size_t i = 0; i < events_.size(); ++i) {
                auto [index, ev_t] = events_[i];
                if (!fired_[i] && ev_t <= t) {
                    fired_[i] = true;
                    ++fired_count_;
                    frame_events_.emplace_back(index, ev_t);
                }
            }
        }
        judged_.clear();
        results_.clear();
        bool broken = false;
        for (const auto &j : judged) {
            const auto &n = notes_.at(j.note_id);
            int32_t conv = conversion_.convert(j.judgement, n.judgement_type, j.time_ms);
            combo_.add_judgement(n.time_ms, conv);
            results_.emplace_back(n, conv);
            judged_.push_back({n.id, conv, n.time_ms});
            if (conv == 1 || conv == 2)
                broken = true;
        }
        int32_t simulator_combo = combo_.timing_combo(t);
        int32_t added_combo = std::max(0, simulator_combo - simulator_previous_combo_);
        simulator_previous_combo_ = simulator_combo;
        if (broken)
            current_combo_ = 0;
        current_combo_ = wrap_add(current_combo_, added_combo);

        const auto &note_percent = score.calc.settings.note_percent;
        for (const auto &[n, conv] : results_) {
            life.note_damage(n.time_ms, conv);
            int32_t lf = life.life_at(n.time_ms);
            if (note_percent.count(n.op))
                score.add_note(n.time_ms, lf, n.id, n.op, convert_score_type(conv));
        }
        score.calculate(t, combo_, info());

        FrameInput inp{t, params_.music_length_ms, finished_};
        for (size_t si : enabled_live_)
            live_[si].trigger = {false, 0};
        bool started = false;
        if (!frame_events_.empty()) {
            for (auto &pool : live_pools_) {
                std::optional<std::pair<int32_t, int32_t>> ev;
                for (ptrdiff_t i = static_cast<ptrdiff_t>(frame_events_.size()) - 1; i >= 0; --i) {
                    if (frame_events_[static_cast<size_t>(i)].first == pool.member) {
                        ev = frame_events_[static_cast<size_t>(i)];
                        break;
                    }
                }
                if (!ev.has_value())
                    continue;
                size_t si = pool.available.front();
                pool.available.erase(pool.available.begin());
                auto &s = live_[si];
                s.trigger = {true, ev->second};
                for (auto &e : s.effects)
                    e.state.state = kStay;
                enabled_live_.push_back(si);
                started = true;
            }
        }
        if (started && enabled_live_.size() > 1) {
            std::sort(enabled_live_.begin(), enabled_live_.end(), [&](size_t a, size_t b) {
                return (live_[a].key * 10 + live_[a].index) < (live_[b].key * 10 + live_[b].index);
            });
        }
        for (auto &c : cond_)
            c.updater.begin_frame();
        ctx_.frame_time = t;
        ctx_.current_combo = current_combo_;
        ctx_.prev_confirmed_rank = frame_rank;
        if (ctx_.gk) {
            ctx_.gk->prev_lots = gk_->prev_lots;
            ctx_.gk->prev_lot_ms = gk_->prev_lot_ms;
        }
        for (int32_t ph = 1; ph <= 2; ++ph) {
            std::vector<Listed> listed;
            for (size_t si : enabled_live_) {
                auto &s = live_[si];
                if (s.parent_state == kEndFrame) {
                    s.parent_state = kStay;
                    for (auto &e : s.effects)
                        e.state.state = kStay;
                    continue;
                }
                bool changed = false;
                for (auto &e : s.effects) {
                    if (e.phase == ph)
                        changed = update_live_effect(e, inp, s.trigger, ctx_) || changed;
                }
                if (changed)
                    s.parent_state = aggregate_live_state(s.effects);
                for (size_t ei = 0; ei < s.effects.size(); ++ei) {
                    if (s.effects[ei].state.state != kStay && s.effects[ei].phase == ph)
                        listed.push_back({true, si, ei});
                }
            }
            for (size_t ui = 0; ui < cond_.size(); ++ui) {
                auto &updater = cond_[ui].updater;
                for (size_t x : updater.update(ph, inp, ctx_)) {
                    if (updater.updaters[x].state.state != kStay)
                        listed.push_back({false, ui, x});
                }
            }
            for (const auto &item : listed)
                apply(item);
        }
        if (!enabled_live_.empty()) {
            bool stayed = false;
            for (size_t si : enabled_live_) {
                const auto &s = live_[si];
                if (s.parent_state == kStay) {
                    for (auto &p : live_pools_) {
                        if (p.key == s.key && p.member == s.member) {
                            p.available.push_back(si);
                            break;
                        }
                    }
                    stayed = true;
                }
            }
            if (stayed) {
                enabled_live_.erase(
                    std::remove_if(enabled_live_.begin(), enabled_live_.end(),
                                   [&](size_t si) { return live_[si].parent_state == kStay; }),
                    enabled_live_.end());
            }
        }
        life.sync_current(t);
        score.calculate(t, combo_, info());
        if (gk_)
            gekisou_after(t);
    }

private:
    struct EffectRow {
        int32_t id = 0;
        int32_t type = 0;
        int32_t value = 0;
        int32_t max_value = 0;
        int32_t limit_count = 0;
        std::vector<int32_t> targets;
    };

    struct LiveEffect {
        size_t row = 0;
        float act = 0.0f;
        int32_t phase = 1;
        EffectState state;
        std::shared_ptr<Checker> condition;
        std::shared_ptr<Checker> release;
        std::shared_ptr<Cumulative> cumulative;
    };

    struct LiveSkill {
        int32_t key = 0;
        int32_t member = 0;
        int32_t index = 0;
        int32_t parent_state = kStay;
        Trigger trigger;
        std::vector<LiveEffect> effects;
    };

    struct LivePool {
        int32_t key = 0;
        int32_t member = 0;
        std::vector<size_t> available;
    };

    struct CondSkill {
        int32_t member = 0;
        int32_t skill_type = 0;
        ConditionSkillUpdater updater;
    };

    struct Listed {
        bool live = false;
        size_t idx1 = 0; // skill or updater
        size_t idx2 = 0; // effect or u
    };

    struct GekisouLive {
        Controller ctrl;
        FeverUpdater fever;
        std::array<std::array<int32_t, 5>, 3> factors;
        std::vector<std::pair<int32_t, int32_t>> fever_updates;
        std::vector<int32_t> prev_lots;
        int32_t prev_lot_ms = 0;

        GekisouLive(Controller c, FeverUpdater f, std::array<std::array<int32_t, 5>, 3> fac)
            : ctrl(std::move(c)), fever(std::move(f)), factors(fac) {}
    };

    const GekisouComboInfo *info() const {
        return gk_ ? &gk_->ctrl : nullptr;
    }

    static int32_t aggregate_live_state(const std::vector<LiveEffect> &effects) {
        for (int32_t st : {kExecuting, kExecuteFrame, kEndFrame}) {
            for (const auto &e : effects)
                if (e.state.state == st)
                    return st;
        }
        return kStay;
    }

    static bool update_live_effect(LiveEffect &e, const FrameInput &inp, const Trigger &trigger,
                                   CheckCtx &ctx) {
        int32_t before = e.state.state;
        bool changed = effect_update(e.state, e.act, inp, trigger, e.condition.get(),
                                     e.release.get(), false, ctx);
        if (e.cumulative) {
            int32_t after = e.state.state;
            if ((before == kStay && after == kExecuteFrame) || before == kExecuteFrame ||
                (before == kExecuting && after == kExecuting))
                e.state.cumulative_count = e.cumulative->update_count(ctx);
            else if (before == kEndFrame)
                e.cumulative->reset();
        }
        return changed;
    }

    void gekisou_after(int32_t t) {
        auto &gk = *gk_;
        std::vector<GkNote> judged;
        judged.reserve(results_.size());
        for (const auto &[n, j] : results_)
            judged.push_back({n.id, n.op, n.time_ms, j});
        if (record_) {
            size_t frame = record_->frames.size() - 1;
            for (size_t r = 0; r < gk.ctrl.ranges.size(); ++r) {
                const auto &range = gk.ctrl.ranges[r];
                if (range.mission != 2)
                    continue;
                for (const auto &[id, op, time_ms, judgement] : judged) {
                    if (range.targets.count(id) && judgement != 0 && judgement != 7) {
                        record_->notes.push_back({frame, static_cast<int32_t>(r), op, judgement,
                                                  time_ms, gk.ctrl.gauge_speed_at(time_ms),
                                                  gk.ctrl.lot_buff_at(time_ms)});
                    }
                }
            }
        }
        GkEnv env{random_, scorectl_};
        gk.ctrl.update(t, judged, gk.fever_updates, score.score, env);
        gk.prev_lots = gk.ctrl.lot_results;
        gk.prev_lot_ms = gk.prev_lots.empty() ? 0 : t;
        std::optional<int32_t> comp_idx;
        for (int32_t i : gk.ctrl.state_updates) {
            if (gk.ctrl.states[i].state == kSComplete) {
                comp_idx = i;
                break;
            }
        }
        if (!comp_idx.has_value())
            return;
        int32_t idx = *comp_idx;
        const auto &range = gk.ctrl.ranges[idx];
        int32_t s0 = score.calculate(range.start_ms, combo_, &gk.ctrl);
        int32_t s1 = score.calculate(range.end_ms, combo_, &gk.ctrl);
        gk.ctrl.states[idx].start_score = s0;
        gk.ctrl.states[idx].end_score = s1;
        if (static_cast<size_t>(idx) < gk.factors.size()) {
            int32_t percent = gk.factors[idx][0];
            int64_t p = static_cast<int64_t>(gk.ctrl.states[idx].score()) * percent;
            int32_t bonus = static_cast<int32_t>(p >= 0 ? (p / 100) : -((-p) / 100));
            score.add_fixed(range.end_ms, bonus);
            prev_confirmed_rank_ = 1;
        }
    }

    EffectState &state_of(const Listed &item) {
        if (item.live)
            return live_[item.idx1].effects[item.idx2].state;
        return cond_[item.idx1].updater.updaters[item.idx2].state;
    }

    void apply(const Listed &item) {
        size_t ri = 0;
        int32_t k = 0;
        int32_t owner_type = 0;
        int32_t skill_type = 0;
        std::string key;
        if (item.live) {
            const auto &s = live_[item.idx1];
            ri = s.effects[item.idx2].row;
            k = s.member;
            owner_type = 1;
            skill_type = 1;
            key = "L" + std::to_string(rows_[ri].id) + ":" + std::to_string(s.member) + ":" +
                  std::to_string(s.index);
        } else {
            const auto &c = cond_[item.idx1];
            const auto &upd = c.updater.updaters[item.idx2];
            const auto &ef = c.updater.effects[upd.effect];
            ri = ef.row;
            k = c.member;
            owner_type = (c.skill_type == 4) ? 1 : 2;
            skill_type = c.skill_type;
            key = "C" + std::to_string(item.idx1) + ":" + std::to_string(ef.effect_id) + ":" +
                  std::to_string(upd.index);
        }
        auto &st = state_of(item);
        int32_t owner = wrap_add(wrap_mul(k, 100), owner_type);
        bool gk_skill = (skill_type == 4 || skill_type == 5);
        const auto &row = rows_[ri];
        int32_t type = row.type;
        bool gk_type = is_gekisou_applier_type(type);
        if ((gk_type || type == 13001) && !gk_skill && !gk_)
            return;
        if (record_ && (type == 11003 || type == 11005)) {
            if (st.state == kExecuteFrame && !item.live) {
                const auto &c = cond_[item.idx1];
                const auto &ef = c.updater.effects[c.updater.updaters[item.idx2].effect];
                record_->actions.push_back({record_->frames.size() - 1,
                                            gk_->ctrl.playing_range_index(), type, row.value,
                                            row.limit_count, chance_of(ef.condition, ctx_)});
            }
            return;
        }
        if ((gk_skill || gk_type) && apply_gekisou(row, st, owner, key))
            return;
        int32_t value = row.value;
        switch (type) {
        case 0:
        case 1000:
        case 1001:
        case 1002:
        case 1003:
        case 1500:
        case 1501:
        case 1502:
        case 1503:
            return;
        case 2000:
        case 2005: {
            int32_t m = note_factor_mill(f32(f32(value) / (type == 2005 ? -10000.0f : 10000.0f)));
            if (st.state == kExecuteFrame)
                score.add_factor({st.execute_ms, owner, m, 0, 0, 0, 0});
            else if (st.state == kEndFrame)
                score.add_factor({st.finish_ms, owner, wrap_sub(0, m), 0, 0, 0, 0});
            return;
        }
        case 2001:
        case 2003:
            apply_cumulative_score(row, st, owner, key);
            return;
        case 2002: {
            int32_t m = judgement_factor_mill(f32(f32(value) / 10000.0f));
            if (st.state == kExecuteFrame)
                score.add_factor({st.execute_ms, owner, 0, m, 0, 0, 0});
            else if (st.state == kEndFrame)
                score.add_factor({st.finish_ms, owner, 0, wrap_sub(0, m), 0, 0, 0});
            return;
        }
        case 2004: {
            int32_t m = judgement_factor_mill(f32(f32(value) / 10000.0f));
            for (int32_t j : row.targets) {
                if (st.state == kExecuteFrame)
                    score.add_factor({st.execute_ms, owner, 0, 0, j, m, 0});
                else if (st.state == kEndFrame)
                    score.add_factor({st.finish_ms, owner, 0, 0, j, wrap_sub(0, m), 0});
            }
            return;
        }
        case 15000:
            if (st.state == kExecuteFrame)
                extend(k, f32(value));
            return;
        case 3000:
            if (st.state == kExecuteFrame) {
                limits_[key] = life.add_limit(value);
            } else if (st.state == kEndFrame) {
                auto it = limits_.find(key);
                if (it != limits_.end()) {
                    life.subtract_limit(it->second);
                    limits_.erase(it);
                }
            }
            return;
        case 3002:
            if (st.state == kExecuteFrame)
                life.skill_damage(st.execute_ms, value, true);
            return;
        case 3004:
            if (st.state == kExecuteFrame) {
                reductions_[key] = life.enable_reduction(st.execute_ms, value);
            } else if (st.state == kEndFrame) {
                auto it = reductions_.find(key);
                if (it != reductions_.end()) {
                    life.disable_reduction(st.finish_ms, it->second);
                    reductions_.erase(it);
                }
            }
            return;
        case 3001:
            if (st.state == kExecuteFrame)
                life.recovery(st.execute_ms, value, true);
            return;
        case 3003:
            if (st.state == kExecuteFrame) {
                guards_[key] = life.enable_guard(st.execute_ms);
            } else if (st.state == kEndFrame) {
                auto it = guards_.find(key);
                if (it != guards_.end()) {
                    life.disable_guard(st.finish_ms, it->second);
                    guards_.erase(it);
                }
            }
            return;
        case 12006:
        case 13005: {
            if (type == 13005 && !gk_)
                return;
            auto t = conversion_.update(key, st.state,
                                        {type, value, row.limit_count, row.targets, row.id});
            if (t.has_value()) {
                st.state = kEndFrame;
                st.finish_ms = *t;
            }
            return;
        }
        default:
            throw std::runtime_error("不支持的技能效果类型: " + std::to_string(type));
        }
    }

    void apply_cumulative_score(const EffectRow &row, EffectState &st, int32_t owner,
                                const std::string &key) {
        bool is_combo = (row.type == 2003);
        int32_t val = wrap_mul(row.value, st.cumulative_count);
        if (row.max_value > 0)
            val = std::min(val, row.max_value);
        float factor = f32(f32(val) / 10000.0f);
        auto add_fn = [&](int32_t t) {
            return is_combo ? scorectl_.add_combo_bonus(owner, t, factor)
                            : scorectl_.add_note_score_up(owner, t, factor);
        };
        auto dis_fn = [&](int32_t t, int32_t id) {
            if (is_combo)
                scorectl_.disable_combo_bonus(t, id);
            else
                scorectl_.disable_note_score_up(t, id);
        };
        if (st.state == kExecuteFrame) {
            ga_.add(key, add_fn(st.execute_ms));
        } else if (st.state == kExecuting) {
            int32_t id = ga_.ids.at(key);
            float old = is_combo ? scorectl_.combo_bonus_factor(id) : scorectl_.score_up_factor(id);
            if (!approximately(old, factor)) {
                dis_fn(frame_time_, id);
                ga_.ids[key] = add_fn(frame_time_);
            }
        } else if (st.state == kEndFrame) {
            dis_fn(st.finish_ms, ga_.pop(key));
        }
    }

    bool apply_gekisou(const EffectRow &row, EffectState &st, int32_t owner,
                       const std::string &key) {
        int32_t et = row.type;
        int32_t v = row.value;
        auto &c = gk_->ctrl;
        int32_t s = st.state;
        switch (et) {
        case 12000:
        case 13000:
            if (s == kExecuteFrame) {
                ga_.add(key, et == 12000 ? c.add_combo_bonus(st.execute_ms, f32(v))
                                         : c.add_just_bonus(st.execute_ms, f32(v)));
            } else if (s == kEndFrame) {
                int32_t id = ga_.pop(key);
                if (et == 12000)
                    c.subtract_combo_bonus(st.finish_ms, id);
                else
                    c.subtract_just_bonus(st.finish_ms, id);
            }
            return true;
        case 13002:
            if (s == kEndFrame) {
                auto it = ga_.ids.find(key);
                if (it != ga_.ids.end()) {
                    c.remove_cumulative_rule(it->second);
                    ga_.ids.erase(it);
                }
            } else if (s == kExecuteFrame && !ga_.ids.count(key)) {
                ga_.ids[key] =
                    c.add_cumulative_rule(st.cumulative_unit, v, st.cumulative_max, row.max_value);
            }
            return true;
        case 11000:
        case 11001:
            if (s == kExecuteFrame) {
                float val = f32(f32(v) / 10000.0f);
                ga_.add(key, et == 11000 ? c.add_lot_probability_up(st.execute_ms, val)
                                         : c.add_gauge_up(st.execute_ms, val));
            } else if (s == kEndFrame) {
                int32_t id = ga_.pop(key);
                if (et == 11000)
                    c.subtract_lot_probability_up(st.finish_ms, id);
                else
                    c.subtract_gauge_up(st.finish_ms, id);
            }
            return true;
        case 11002:
        case 11004:
        case 12002:
        case 12003:
        case 13003:
        case 13004:
            if (s == kExecuteFrame) {
                int32_t count = v;
                if (et == 11004 || et == 12003 || et == 13004) {
                    count = wrap_mul(count, st.cumulative_count);
                    if (row.max_value > 0)
                        count = std::min(count, row.max_value);
                }
                if (et == 11002 || et == 11004)
                    c.add_luck_point(count);
                else if (et == 12002 || et == 12003)
                    c.add_gekisou_combo(frame_time_, count);
                else
                    c.add_just_count(frame_time_, count);
            }
            return true;
        case 11003:
            if (s == kExecuteFrame) {
                int32_t g = wrap_mul(c.current_gauge_max(), v);
                c.add_luck_gauge_percent(floor_to_i32(f32(f32(g) / 10000.0f)));
            }
            return true;
        case 12004:
            ga_.limit(key, st);
            if (st.state == kExecuteFrame) {
                int32_t mask = 0;
                for (int32_t j : row.targets)
                    if (j != 0 && j != -1)
                        mask |= (1 << (j & 31));
                ga_.add(key, c.enable_combo_protect(st.execute_ms, row.limit_count, mask & 0xff));
            } else if (st.state == kEndFrame && !ga_.exhausted.erase(key)) {
                auto it = ga_.ids.find(key);
                if (it != ga_.ids.end()) {
                    c.disable_combo_protect(st.finish_ms, it->second);
                    ga_.ids.erase(it);
                }
            }
            return true;
        case 11005:
            ga_.limit(key, st);
            if (st.state == kExecuteFrame) {
                ga_.add(key, c.machine.enable_minimum(minimum_result_of(v), row.limit_count));
            } else if (st.state == kExecuting) {
                auto it = ga_.ids.find(key);
                if (it != ga_.ids.end() && !c.machine.is_minimum_active(it->second)) {
                    int32_t id = it->second;
                    int32_t t = c.machine.take_last_consumed(id);
                    if (t < 0)
                        t = frame_time_;
                    c.machine.disable_minimum(id);
                    ga_.ids.erase(it);
                    ga_.exhausted.insert(key);
                    ga_.limit_finished[key] = t;
                }
            } else if (st.state == kEndFrame && !ga_.exhausted.erase(key)) {
                auto it = ga_.ids.find(key);
                if (it != ga_.ids.end()) {
                    c.machine.disable_minimum(it->second);
                    ga_.ids.erase(it);
                }
            }
            return true;
        case 2001: {
            int32_t n = wrap_mul(v, st.cumulative_count);
            if (row.max_value > 0 && row.max_value < n)
                n = row.max_value;
            float factor = f32(f32(n) / 10000.0f);
            if (s == kExecuteFrame) {
                ga_.add(key, scorectl_.add_note_score_up(owner, st.execute_ms, factor));
            } else if (s == kExecuting) {
                int32_t id = ga_.ids.at(key);
                if (!approximately(scorectl_.score_up_factor(id), factor)) {
                    scorectl_.disable_note_score_up(frame_time_, id);
                    ga_.ids[key] = scorectl_.add_note_score_up(owner, frame_time_, factor);
                }
            } else if (s == kEndFrame) {
                scorectl_.disable_note_score_up(st.finish_ms, ga_.pop(key));
            }
            return true;
        }
        case 4004:
            return true;
        default:
            return false;
        }
    }

    void extend(int32_t member, float ms) {
        for (auto &s : live_) {
            if (s.member != member)
                continue;
            for (auto &e : s.effects) {
                if (e.state.state == kExecuteFrame || e.state.state == kExecuting)
                    e.state.extended_ms = f32(e.state.extended_ms + ms);
            }
        }
    }

    std::unordered_map<int32_t, LiveNote> notes_;
    std::vector<std::pair<int32_t, int32_t>> events_;
    std::vector<bool> fired_;
    LiveRandom random_;
    ComboCounter combo_;
    Conversion conversion_;
    LiveParams params_;
    std::vector<EffectRow> rows_;
    std::vector<LiveSkill> live_;
    std::vector<LivePool> live_pools_;
    std::vector<size_t> enabled_live_;
    std::vector<CondSkill> cond_;
    std::unordered_map<std::string, int32_t> guards_;
    std::unordered_map<std::string, int32_t> reductions_;
    std::unordered_map<std::string, int32_t> limits_;
    std::vector<std::pair<int32_t, int32_t>> frame_events_;
    std::vector<std::array<int32_t, 3>> judged_;
    std::vector<std::pair<LiveNote, int32_t>> results_;
    size_t fired_count_ = 0;
    CheckCtx ctx_;
    int32_t frame_time_ = 0;
    std::optional<int32_t> prev_confirmed_rank_;
    int32_t simulator_previous_combo_ = 0;
    int32_t current_combo_ = 0;
    ScoreCtl scorectl_;
    std::unique_ptr<GekisouLive> gk_;
    GkAppliers ga_;
    LuckRecord *record_ = nullptr;
    bool finished_ = false;
};

} // namespace

std::pair<float, std::vector<MissRule>> compute_luck_readers(const EngineMaster &master,
                                                             const std::vector<Performer> &deck) {
    Factory factory(master, deck, master.live.life_base, 0);
    LifeController life(master.live.life_base, master.live.damage, 1000);
    LiveRandom random(0);
    std::vector<std::array<int32_t, 3>> empty_j;
    std::vector<std::pair<int32_t, int32_t>> empty_ev;
    CheckCtx ctx{std::nullopt, &life, &random, 0, 0, &empty_j, &empty_ev, std::nullopt, true};

    auto reads = [&](int32_t group, int32_t type, std::optional<int32_t> val = std::nullopt) {
        auto it = master.condition_sets.find(group);
        if (it == master.condition_sets.end())
            return false;
        for (const auto &set : it->second) {
            for (int32_t id : set) {
                auto cit = master.conditions.find(id);
                if (cit != master.conditions.end() && cit->second.type == type) {
                    if (!val.has_value() ||
                        (!cit->second.values.empty() && cit->second.values[0] == *val))
                        return true;
                }
            }
        }
        return false;
    };

    float rush = 0.0f;
    std::vector<MissRule> misses;
    for (size_t k = 0; k < deck.size(); ++k) {
        const auto &p = deck[k];
        if (!p.gekisou_skill.has_value() && (!p.mission_only.has_value() || *p.mission_only == 0))
            continue;
        for (const auto &[id, level] : p.gekisou_support_skills) {
            auto sit = master.gekiso_support_skills.find(id);
            if (sit == master.gekiso_support_skills.end())
                continue;
            const auto &skill = sit->second;
            if (skill.mission_type != kMLuck && skill.mission_type != kMAll)
                continue;
            for (const auto &row : skill.effects) {
                if (row.level != level)
                    continue;
                if (row.type == 2000 && reads(row.trigger_group, 7021)) {
                    if (chance_of(factory.group(row.condition_group, k), ctx) > 0.5) {
                        rush = f32(rush + f32(static_cast<float>(note_factor_mill(
                                                  f32(f32(row.value) / 10000.0f))) /
                                              100000.0f));
                    }
                } else if (row.type == 11003 && reads(row.trigger_group, 7000, 0)) {
                    double ch = chance_of(factory.group(row.condition_group, k), ctx);
                    if (ch > 0.0)
                        misses.push_back({row.value, ch});
                }
            }
        }
    }
    return {rush, std::move(misses)};
}

void simulate_live_span(
    const EngineMaster &master, const std::vector<Performer> &deck,
    const std::vector<LiveNote> &notes, const std::vector<std::pair<int32_t, int32_t>> &events,
    const LiveParams &params, const GekisouSetup *setup, int32_t seed,
    const std::vector<PlayFrame> &frames, const std::vector<float> &dt, size_t span_begin,
    size_t span_end, LuckRecord *record,
    const std::function<void(int32_t, int32_t, float, float, float, float)> &on_note) {
    LiveModel model(master, deck, notes, events, params, setup, seed, record);
    model.score.calc.on_note = on_note;
    for (size_t k = span_begin; k < span_end; ++k) {
        model.frame(frames[k].time_ms, frames[k].judged, dt[k]);
    }
}

} // namespace gekisou_detail
} // namespace deckcalc
