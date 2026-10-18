use isolang::Language;

pub fn normalize(value: &str) -> String {
    let value = value.trim().to_lowercase();
    let base = value.split(['-', '_']).next().unwrap_or(&value);
    let lang = Language::from_639_1(base)
        .or_else(|| Language::from_639_3(base))
        .or_else(|| {
            isolang::languages().find(|language| language.to_name().eq_ignore_ascii_case(&value))
        });
    lang.map(|l| l.to_639_1().unwrap_or(l.to_639_3()).to_owned())
        .unwrap_or(value)
}
pub fn detect(text: &str) -> Option<String> {
    whatlang::detect(text).map(|info| normalize(info.lang().code()))
}
pub fn target(
    source: Option<&str>,
    preferred: &str,
    fallback: &str,
    override_target: Option<&str>,
) -> String {
    if let Some(value) = override_target.filter(|s| !s.trim().is_empty() && *s != "auto") {
        return normalize(value);
    }
    if source.is_some_and(|s| normalize(s) == normalize(preferred)) {
        normalize(fallback)
    } else {
        normalize(preferred)
    }
}
