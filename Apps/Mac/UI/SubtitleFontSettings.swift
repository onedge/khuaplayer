import AppKit

// Process-wide subtitle font preference. Unlike subtitle scale, which belongs
// to one playback session, a font choice is a reading preference: it persists
// across launches and applies to every open window through `changed`.
// nil keeps the default style font of each subtitle track.
enum SubtitleFontSettings {
    static let key = "sp.subtitle.fontFamily"
    static let changed = Notification.Name("sp.subtitleFontChanged")

    static var family: String? {
        get {
            guard let name = UserDefaults.standard.string(forKey: key),
                  !name.isEmpty else { return nil }
            return name
        }
        set {
            let name = newValue.flatMap { $0.isEmpty ? nil : $0 }
            guard name != family else { return }
            if let name {
                UserDefaults.standard.set(name, forKey: key)
            } else {
                UserDefaults.standard.removeObject(forKey: key)
            }
            NotificationCenter.default.post(name: changed, object: nil)
        }
    }
}

// Display names for font families. libass matches the raw family name
// through CoreText; the localized name is for display only.
enum SubtitleFontCatalog {
    static func displayName(for family: String) -> String {
        NSFontManager.shared.localizedName(forFamily: family, face: nil)
    }
}

// Subtitle Font menu contents. Items are rebuilt when the menu opens so the
// list follows font installation; All Fonts is populated only when opened
// because a typical Mac lists several hundred families.
enum SubtitleFontMenu {
    static let identifier = "sp.subtitleFont"
    static let allFontsIdentifier = "sp.subtitleFont.all"

    /// Delivers the default style font and nearby subtitle text on the main actor.
    typealias FontSampleRequest = (@escaping @MainActor (String?, String?) -> Void) -> Void

    @MainActor
    static func populate(_ menu: NSMenu, requestSample: FontSampleRequest?) {
        menu.removeAllItems()
        let current = SubtitleFontSettings.family
        let defaultItem = item(title: L("menu.subtitleFont.default"), family: nil, current: current)
        menu.addItem(defaultItem)
        if let requestSample {
            describeDefault(defaultItem, requestSample: requestSample)
        }

        // A family chosen under All Fonts stays visible here, including one
        // that was uninstalled later and now falls back to another font.
        if let current {
            menu.addItem(.separator())
            menu.addItem(item(title: SubtitleFontCatalog.displayName(for: current),
                              family: current, current: current))
        }

        menu.addItem(.separator())
        let allItem = NSMenuItem(title: L("menu.subtitleFont.all"), action: nil, keyEquivalent: "")
        let allMenu = NSMenu(title: L("menu.subtitleFont.all"))
        allMenu.identifier = NSUserInterfaceItemIdentifier(allFontsIdentifier)
        allMenu.delegate = menu.delegate
        allItem.submenu = allMenu
        menu.addItem(allItem)
    }

    /// Renames Default to the families it draws with, e.g. "Default (Arial ·
    /// Apple SD Gothic Neo)". Resolution runs off the main thread; an open
    /// menu shows the new title as soon as it arrives.
    @MainActor
    private static func describeDefault(_ item: NSMenuItem, requestSample: FontSampleRequest) {
        requestSample { [weak item] styleFont, sample in
            guard let styleFont, let sample else { return }
            // Only strings cross to the detached task; the menu item stays on
            // the main actor.
            Task { [weak item] in
                let families = await Task.detached(priority: .userInitiated) {
                    SubtitleFontResolver.resolvedFamilies(styleFont: styleFont, sample: sample)
                }.value
                guard let item, !families.isEmpty else { return }
                let names = families.map(SubtitleFontCatalog.displayName(for:))
                item.title = L("menu.subtitleFont.defaultResolved",
                               names.joined(separator: " · "))
            }
        }
    }

    @MainActor
    static func populateAllFonts(_ menu: NSMenu) {
        menu.removeAllItems()
        let current = SubtitleFontSettings.family
        for family in NSFontManager.shared.availableFontFamilies {
            menu.addItem(item(title: SubtitleFontCatalog.displayName(for: family),
                              family: family, current: current))
        }
    }

    private static func item(title: String, family: String?, current: String?) -> NSMenuItem {
        let item = NSMenuItem(title: title,
                              action: #selector(AppDelegate.subtitleFontAction(_:)),
                              keyEquivalent: "")
        item.representedObject = family
        item.state = family == current ? .on : .off
        return item
    }
}
