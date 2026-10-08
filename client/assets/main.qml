import bb.cascades 1.2

// Jukeberry: three tabs (Now Playing, Library, Search) over the C++ `player`
// (src/player.hpp). Dark theme (CASCADES_THEME=dark in bar-descriptor.xml).
TabbedPane {
    id: root
    showTabsOnActionBar: true

    property variant accent: Color.create("#E0457B")
    property variant muted: Color.create("#A8A8A8")

    Menu.definition: MenuDefinition {
        settingsAction: SettingsActionItem {
            onTriggered: settingsSheet.open()
        }
    }

    onCreationCompleted: {
        if (!player.configured) settingsSheet.open()
    }

    function showNowPlaying() {
        root.activeTab = nowTab
    }

    // A context (playlist, album, artist) opened from a list: its tracks.
    function openTracks(nav, uri, title) {
        var page = tracksPageDefinition.createObject()
        nav.push(page)
        player.openContext(uri, title)
    }

    attachedObjects: [
        ComponentDefinition {
            id: tracksPageDefinition
            Page {
                id: tracksPage
                titleBar: TitleBar {
                    title: player.tracksTitle
                }
                actions: [
                    ActionItem {
                        title: qsTr("Play")
                        imageSource: "asset:///images/ic_play.png"
                        ActionBar.placement: ActionBarPlacement.OnBar
                        onTriggered: {
                            player.play(player.tracksUri)
                            root.showNowPlaying()
                        }
                    }
                ]
                Container {
                    layout: DockLayout {}
                    ListView {
                        dataModel: player.tracks
                        listItemComponents: [
                            ListItemComponent {
                                StandardListItem {
                                    title: ListItemData.name
                                    description: ListItemData.subtitle
                                    status: ListItemData.duration
                                    imageSource: ListItemData.thumb ? ListItemData.thumb : "asset:///images/ic_playlist.png"
                                }
                            }
                        ]
                        onTriggered: {
                            var item = dataModel.data(indexPath)
                            player.play(item.context, item.uri)
                            root.showNowPlaying()
                        }
                    }
                    ActivityIndicator {
                        running: player.tracksLoading
                        visible: running
                        preferredWidth: 120
                        horizontalAlignment: HorizontalAlignment.Center
                        verticalAlignment: VerticalAlignment.Center
                    }
                }
            }
        },
        Sheet {
            id: settingsSheet
            onOpened: {
                serverField.text = player.serverUrl
                tokenField.text = player.token
            }
            Page {
                titleBar: TitleBar {
                    title: qsTr("Settings")
                    dismissAction: ActionItem {
                        title: qsTr("Cancel")
                        onTriggered: settingsSheet.close()
                    }
                    acceptAction: ActionItem {
                        title: qsTr("Save")
                        onTriggered: {
                            player.saveSettings(serverField.text, tokenField.text)
                            settingsSheet.close()
                        }
                    }
                }
                ScrollView {
                    Container {
                        leftPadding: 30
                        rightPadding: 30
                        topPadding: 30
                        Label {
                            text: qsTr("Jukeberry server")
                        }
                        TextField {
                            id: serverField
                            hintText: qsTr("e.g. https://jukeberry.example.com")
                            inputMode: TextFieldInputMode.Url
                        }
                        Label {
                            text: qsTr("Access token (server/data/token on the server)")
                            multiline: true
                        }
                        TextField {
                            id: tokenField
                            hintText: qsTr("Token")
                            inputMode: TextFieldInputMode.Password
                        }
                        Label {
                            topMargin: 40
                            text: qsTr("Jukeberry plays Spotify through go-librespot on your own server. It needs Spotify Premium. Not affiliated with Spotify.")
                            multiline: true
                            textStyle.base: SystemDefaults.TextStyles.SmallText
                            textStyle.color: root.muted
                        }
                    }
                }
            }
        }
    ]

    // --- Now Playing -------------------------------------------------------
    Tab {
        id: nowTab
        title: qsTr("Now Playing")
        imageSource: "asset:///images/ic_tab_now.png"
        Page {
            Container {
                background: Color.create("#121212")
                layout: DockLayout {}
                Container {
                    horizontalAlignment: HorizontalAlignment.Fill
                    verticalAlignment: VerticalAlignment.Center
                    leftPadding: 40
                    rightPadding: 40

                    Label {
                        visible: player.message != ""
                        text: player.message
                        multiline: true
                        horizontalAlignment: HorizontalAlignment.Center
                        textStyle.color: root.accent
                        textStyle.textAlign: TextAlign.Center
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                    }
                    ImageView {
                        id: coverView
                        imageSource: player.cover != "" ? player.cover : "asset:///images/ic_playlist.png"
                        preferredWidth: 270
                        preferredHeight: 270
                        scalingMethod: ScalingMethod.AspectFit
                        horizontalAlignment: HorizontalAlignment.Center
                    }
                    Container {
                        topPadding: 16
                        layout: StackLayout {
                            orientation: LayoutOrientation.LeftToRight
                        }
                        Container {
                            layoutProperties: StackLayoutProperties {
                                spaceQuota: 1
                            }
                            Label {
                                text: player.hasTrack ? player.title : qsTr("Nothing playing")
                                textStyle.base: SystemDefaults.TextStyles.TitleText
                                textStyle.color: Color.White
                            }
                            Label {
                                text: player.artists
                                visible: player.hasTrack
                                textStyle.color: root.muted
                            }
                        }
                        ImageButton {
                            visible: player.hasTrack
                            verticalAlignment: VerticalAlignment.Center
                            defaultImageSource: player.liked ? "asset:///images/ic_liked.png" : "asset:///images/ic_like.png"
                            onClicked: player.toggleLike()
                        }
                    }
                    Slider {
                        id: progress
                        fromValue: 0
                        toValue: player.duration > 0 ? player.duration : 1
                        enabled: player.hasTrack
                        horizontalAlignment: HorizontalAlignment.Fill
                        // Moved by the user: seek. (Updates from the player
                        // land within a second of the real position, so they
                        // never look like a seek.)
                        onValueChanged: {
                            if (Math.abs(value - player.position) > 2500) player.seekTo(value)
                        }
                    }
                    Container {
                        layout: DockLayout {}
                        horizontalAlignment: HorizontalAlignment.Fill
                        Label {
                            text: player.formatTime(player.position)
                            textStyle.base: SystemDefaults.TextStyles.SmallText
                            textStyle.color: root.muted
                        }
                        Label {
                            horizontalAlignment: HorizontalAlignment.Right
                            text: player.formatTime(player.duration)
                            textStyle.base: SystemDefaults.TextStyles.SmallText
                            textStyle.color: root.muted
                        }
                    }
                    Container {
                        horizontalAlignment: HorizontalAlignment.Center
                        layout: StackLayout {
                            orientation: LayoutOrientation.LeftToRight
                        }
                        ImageButton {
                            verticalAlignment: VerticalAlignment.Center
                            defaultImageSource: player.shuffle ? "asset:///images/ic_shuffle_on.png" : "asset:///images/ic_shuffle.png"
                            onClicked: player.toggleShuffle()
                        }
                        ImageButton {
                            leftMargin: 20
                            verticalAlignment: VerticalAlignment.Center
                            defaultImageSource: "asset:///images/ic_prev.png"
                            onClicked: player.previous()
                        }
                        ImageButton {
                            leftMargin: 20
                            verticalAlignment: VerticalAlignment.Center
                            defaultImageSource: player.playing ? "asset:///images/ic_pause.png" : "asset:///images/ic_play.png"
                            onClicked: player.playPause()
                        }
                        ImageButton {
                            leftMargin: 20
                            verticalAlignment: VerticalAlignment.Center
                            defaultImageSource: "asset:///images/ic_next.png"
                            onClicked: player.next()
                        }
                        ImageButton {
                            leftMargin: 20
                            verticalAlignment: VerticalAlignment.Center
                            defaultImageSource: player.repeat == 2 ? "asset:///images/ic_repeat_one_on.png"
                                : (player.repeat == 1 ? "asset:///images/ic_repeat_on.png" : "asset:///images/ic_repeat.png")
                            onClicked: player.cycleRepeat()
                        }
                    }
                }
            }
            onCreationCompleted: {
                player.positionChanged.connect(function () {
                    progress.value = player.position
                })
            }
        }
    }

    // --- Library -----------------------------------------------------------
    Tab {
        title: qsTr("Library")
        imageSource: "asset:///images/ic_tab_library.png"
        NavigationPane {
            id: libraryNav
            onPopTransitionEnded: page.destroy()
            Page {
                titleBar: TitleBar {
                    title: qsTr("Your Library")
                }
                actions: [
                    ActionItem {
                        title: qsTr("Refresh")
                        imageSource: "asset:///images/ic_refresh.png"
                        ActionBar.placement: ActionBarPlacement.OnBar
                        onTriggered: player.loadPlaylists()
                    }
                ]
                Container {
                    layout: DockLayout {}
                    ListView {
                        dataModel: player.playlists
                        listItemComponents: [
                            ListItemComponent {
                                StandardListItem {
                                    title: ListItemData.name
                                    description: ListItemData.subtitle
                                    imageSource: ListItemData.thumb ? ListItemData.thumb : "asset:///images/ic_playlist.png"
                                }
                            }
                        ]
                        onTriggered: {
                            var item = dataModel.data(indexPath)
                            root.openTracks(libraryNav, item.uri, item.name)
                        }
                    }
                    ActivityIndicator {
                        running: player.playlistsLoading
                        visible: running
                        preferredWidth: 120
                        horizontalAlignment: HorizontalAlignment.Center
                        verticalAlignment: VerticalAlignment.Center
                    }
                }
            }
        }
    }

    // --- Search ------------------------------------------------------------
    Tab {
        title: qsTr("Search")
        imageSource: "asset:///images/ic_tab_search.png"
        NavigationPane {
            id: searchNav
            onPopTransitionEnded: page.destroy()
            Page {
                titleBar: TitleBar {
                    title: qsTr("Search")
                }
                Container {
                    TextField {
                        id: searchField
                        hintText: qsTr("Songs, artists, albums, playlists")
                        input.submitKey: SubmitKey.Search
                        input.onSubmitted: player.search(text)
                    }
                    Container {
                        layout: DockLayout {}
                        ListView {
                            dataModel: player.results
                            function itemType(data, indexPath) {
                                return data.kind == "header" ? "header" : "item"
                            }
                            listItemComponents: [
                                ListItemComponent {
                                    type: "header"
                                    Header {
                                        title: ListItemData.name
                                    }
                                },
                                ListItemComponent {
                                    type: "item"
                                    StandardListItem {
                                        title: ListItemData.name
                                        description: ListItemData.subtitle
                                        status: ListItemData.duration ? ListItemData.duration : ""
                                        imageSource: ListItemData.thumb ? ListItemData.thumb : "asset:///images/ic_playlist.png"
                                    }
                                }
                            ]
                            onTriggered: {
                                var item = dataModel.data(indexPath)
                                if (item.kind == "header") return
                                if (item.kind == "track") {
                                    player.play(item.uri)
                                    root.showNowPlaying()
                                } else {
                                    root.openTracks(searchNav, item.uri, item.name)
                                }
                            }
                        }
                        ActivityIndicator {
                            running: player.searching
                            visible: running
                            preferredWidth: 120
                            horizontalAlignment: HorizontalAlignment.Center
                            verticalAlignment: VerticalAlignment.Center
                        }
                    }
                }
            }
        }
    }
}
