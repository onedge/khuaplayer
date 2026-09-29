import CoreText

// Names the font families the default subtitle style actually draws with.
// libass's CoreText provider picks glyph fallbacks with CTFontCreateForString
// against the style font (ass_coretext.c get_fallback), so repeating that call
// per character reproduces the families on screen, e.g. Arial for Latin text
// and Apple SD Gothic Neo for Hangul.
enum SubtitleFontResolver {
    /// More families than this read as noise in a menu title.
    static let maxFamilies = 3

    /// Families in order of first appearance in `sample`. Only letters count:
    /// punctuation and emoji often resolve to unrelated fallback fonts.
    static func resolvedFamilies(styleFont: String, sample: String) -> [String] {
        let base = CTFontCreateWithName(styleFont as CFString, 0, nil)
        var seen = Set<Unicode.Scalar>()
        var families: [String] = []
        for scalar in sample.unicodeScalars
        where scalar.properties.isAlphabetic && seen.insert(scalar).inserted {
            let text = String(scalar) as CFString
            let font = CTFontCreateForString(base, text,
                                             CFRange(location: 0, length: CFStringGetLength(text)))
            let family = CTFontCopyFamilyName(font) as String
            guard !families.contains(family) else { continue }
            families.append(family)
            if families.count == maxFamilies { break }
        }
        return families
    }
}
