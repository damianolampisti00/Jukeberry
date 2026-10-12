import bb.cascades 1.2
import bb.system 1.2

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
        Application.setCover(activeFrame)
        player.queued.connect(function () {
            queuedToast.body = qsTr("Added to the queue")
            queuedToast.show()
        })
        player.notice.connect(function (text) {
            queuedToast.body = text
            queuedToast.show()
        })
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

    // "Add to playlist": the playlists the user may edit, to pick one.
    function pickPlaylist(trackUri) {
        var lists = player.editablePlaylists()
        if (lists.length == 0) {
            player.loadPlaylists()
            queuedToast.body = qsTr("No playlist of yours to add to (yet): try again in a moment")
            queuedToast.show()
            return
        }
        playlistPicker.trackUri = trackUri
        playlistPicker.lists = lists
        playlistPicker.clearList()
        for (var i = 0; i < lists.length; ++i) playlistPicker.appendItem(lists[i].name)
        playlistPicker.show()
    }

    attachedObjects: [
        SystemToast {
            id: queuedToast
        },
        SystemListDialog {
            id: playlistPicker
            property string trackUri
            property variant lists
            title: qsTr("Add to playlist")
            selectionMode: ListSelectionMode.Single
            confirmButton.label: qsTr("Add")
            cancelButton.label: qsTr("Cancel")
            onFinished: {
                if (value != SystemUiResult.ConfirmButtonSelection || selectedIndices.length == 0) return
                var pl = lists[selectedIndices[0]]
                player.addToPlaylist(pl.uri, pl.name, trackUri)
            }
        },
        // The play queue: upcoming tracks in play order, the ones added with
        // Add to queue first. A tap jumps to that track.
        Sheet {
            id: queueSheet
            onOpened: player.setQueueVisible(true)
            onClosed: player.setQueueVisible(false)
            Page {
                titleBar: TitleBar {
                    title: qsTr("Queue")
                    dismissAction: ActionItem {
                        title: qsTr("Close")
                        onTriggered: queueSheet.close()
                    }
                }
                actions: [
                    ActionItem {
                        title: qsTr("Clear queue")
                        imageSource: "asset:///images/ic_queue_clear.png"
                        ActionBar.placement: ActionBarPlacement.OnBar
                        onTriggered: player.clearQueue()
                    }
                ]
                Container {
                    layout: DockLayout {}
                    ListView {
                        id: queueList
                        dataModel: player.queue
                        function move(uid, delta) { player.moveInQueue(uid, delta) }
                        function remove(uid) { player.removeFromQueue(uid) }
                        listItemComponents: [
                            ListItemComponent {
                                StandardListItem {
                                    id: queueItem
                                    title: ListItemData.name
                                    description: ListItemData.subtitle
                                    status: ListItemData.queued ? qsTr("In queue") : ListItemData.duration
                                    imageSource: ListItemData.thumb ? ListItemData.thumb : "asset:///images/ic_playlist.png"
                                    // Only the tracks added to the queue can be moved or removed
                                    // (the playlist's own order stays as it is, as on Spotify).
                                    contextActions: [
                                        ActionSet {
                                            ActionItem {
                                                title: qsTr("Move up")
                                                imageSource: "asset:///images/ic_move_up.png"
                                                enabled: ListItemData.queued
                                                onTriggered: queueItem.ListItem.view.move(ListItemData.uid, -1)
                                            }
                                            ActionItem {
                                                title: qsTr("Move down")
                                                imageSource: "asset:///images/ic_move_down.png"
                                                enabled: ListItemData.queued
                                                onTriggered: queueItem.ListItem.view.move(ListItemData.uid, 1)
                                            }
                                            ActionItem {
                                                title: qsTr("Remove from queue")
                                                imageSource: "asset:///images/ic_queue_remove.png"
                                                enabled: ListItemData.queued
                                                onTriggered: queueItem.ListItem.view.remove(ListItemData.uid)
                                            }
                                        }
                                    ]
                                }
                            }
                        ]
                        onTriggered: {
                            var item = dataModel.data(indexPath)
                            player.skipTo(item.uri)
                            queueSheet.close()
                        }
                    }
                    Label {
                        id: queueEmpty
                        visible: false
                        text: qsTr("Nothing coming up")
                        horizontalAlignment: HorizontalAlignment.Center
                        verticalAlignment: VerticalAlignment.Center
                        textStyle.color: root.muted
                    }
                    ActivityIndicator {
                        running: player.queueLoading
                        visible: running
                        preferredWidth: 120
                        horizontalAlignment: HorizontalAlignment.Center
                        verticalAlignment: VerticalAlignment.Center
                    }
                }
            }
            onCreationCompleted: {
                player.queueChanged.connect(function () {
                    queueEmpty.visible = !player.queueLoading && player.queue.size() == 0
                })
            }
        },
        // Active Frame (the minimized app on the home screen, 334x396 on the
        // Q5): the cover full size, title and artist on a dark band.
        SceneCover {
            id: activeFrame
            content: Container {
                background: Color.create("#121212")
                layout: DockLayout {}
                ImageView {
                    visible: player.hasTrack
                    imageSource: player.cover != "" ? player.cover : "asset:///images/ic_playlist.png"
                    horizontalAlignment: HorizontalAlignment.Fill
                    verticalAlignment: VerticalAlignment.Fill
                    scalingMethod: ScalingMethod.AspectFill
                }
                ImageView {
                    visible: !player.hasTrack
                    imageSource: "asset:///icon.png"
                    horizontalAlignment: HorizontalAlignment.Center
                    verticalAlignment: VerticalAlignment.Center
                }
                Container {
                    horizontalAlignment: HorizontalAlignment.Fill
                    verticalAlignment: VerticalAlignment.Bottom
                    background: Color.create("#D0121212")
                    leftPadding: 16
                    rightPadding: 12
                    topPadding: 10
                    bottomPadding: 12
                    layout: StackLayout {
                        orientation: LayoutOrientation.LeftToRight
                    }
                    Container {
                        layoutProperties: StackLayoutProperties {
                            spaceQuota: 1
                        }
                        Label {
                            text: player.hasTrack ? player.title : "Jukeberry"
                            textStyle.base: SystemDefaults.TextStyles.BodyText
                            textStyle.fontWeight: FontWeight.Bold
                            textStyle.color: Color.White
                        }
                        Label {
                            text: player.hasTrack ? player.artists : (player.connected ? qsTr("Nothing playing") : player.message)
                            textStyle.base: SystemDefaults.TextStyles.SmallText
                            textStyle.color: root.muted
                        }
                    }
                    ImageView {
                        visible: player.hasTrack
                        verticalAlignment: VerticalAlignment.Center
                        imageSource: player.playing ? "asset:///images/ic_play.png" : "asset:///images/ic_pause.png"
                        preferredWidth: 56
                        preferredHeight: 56
                    }
                }
            }
        },
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
                        function addToQueue(uri) { player.addToQueue(uri) }
                        function setLiked(uri, liked) { player.setLiked(uri, liked) }
                        function addToPlaylist(uri) { root.pickPlaylist(uri) }
                        listItemComponents: [
                            ListItemComponent {
                                StandardListItem {
                                    id: trackItem
                                    title: ListItemData.name
                                    description: ListItemData.subtitle
                                    status: ListItemData.duration
                                    imageSource: ListItemData.thumb ? ListItemData.thumb : "asset:///images/ic_playlist.png"
                                    // Add to queue (delegates reach player through ListItem.view)
                                    contextActions: [
                                        ActionSet {
                                            ActionItem {
                                                title: qsTr("Add to queue")
                                                imageSource: "asset:///images/ic_queue_add.png"
                                                onTriggered: trackItem.ListItem.view.addToQueue(ListItemData.uri)
                                            }
                                            ActionItem {
                                                title: ListItemData.liked ? qsTr("Remove from Liked Songs") : qsTr("Add to Liked Songs")
                                                imageSource: ListItemData.liked ? "asset:///images/ic_liked.png" : "asset:///images/ic_like.png"
                                                enabled: ListItemData.kind == "track"
                                                onTriggered: trackItem.ListItem.view.setLiked(ListItemData.uri, ! ListItemData.liked)
                                            }
                                            ActionItem {
                                                title: qsTr("Add to playlist")
                                                imageSource: "asset:///images/ic_playlist_add.png"
                                                enabled: ListItemData.kind == "track"
                                                onTriggered: trackItem.ListItem.view.addToPlaylist(ListItemData.uri)
                                            }
                                        }
                                    ]
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
            actions: [
                ActionItem {
                    title: qsTr("Queue")
                    imageSource: "asset:///images/ic_queue.png"
                    ActionBar.placement: ActionBarPlacement.OnBar
                    onTriggered: queueSheet.open()
                },
                ActionItem {
                    title: qsTr("Add to playlist")
                    imageSource: "asset:///images/ic_playlist_add.png"
                    enabled: player.hasTrack
                    onTriggered: root.pickPlaylist(player.trackUri)
                }
            ]
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
                        // While a finger is on it the player doesn't move it,
                        // and lifting the finger seeks. (Seeking on
                        // valueChanged would also fire on the player's own
                        // updates, e.g. when a new track's duration clamps it.)
                        property bool touching: false
                        fromValue: 0
                        toValue: player.duration > 0 ? player.duration : 1
                        enabled: player.hasTrack
                        horizontalAlignment: HorizontalAlignment.Fill
                        onTouch: {
                            if (event.isDown()) {
                                touching = true
                            } else if (event.isUp()) {
                                touching = false
                                player.seekTo(immediateValue)
                            } else if (event.isCancel()) {
                                touching = false
                            }
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
                    if (!progress.touching) progress.value = player.position
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
                            function addToQueue(uri) { player.addToQueue(uri) }
                            function setLiked(uri, liked) { player.setLiked(uri, liked) }
                            function addToPlaylist(uri) { root.pickPlaylist(uri) }
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
                                        id: resultItem
                                        title: ListItemData.name
                                        description: ListItemData.subtitle
                                        status: ListItemData.duration ? ListItemData.duration : ""
                                        imageSource: ListItemData.thumb ? ListItemData.thumb : "asset:///images/ic_playlist.png"
                                        contextActions: [
                                            ActionSet {
                                                ActionItem {
                                                    title: qsTr("Add to queue")
                                                    imageSource: "asset:///images/ic_queue_add.png"
                                                    enabled: ListItemData.kind == "track"
                                                    onTriggered: resultItem.ListItem.view.addToQueue(ListItemData.uri)
                                                }
                                                ActionItem {
                                                    title: ListItemData.liked ? qsTr("Remove from Liked Songs") : qsTr("Add to Liked Songs")
                                                    imageSource: ListItemData.liked ? "asset:///images/ic_liked.png" : "asset:///images/ic_like.png"
                                                    enabled: ListItemData.kind == "track"
                                                    onTriggered: resultItem.ListItem.view.setLiked(ListItemData.uri, ! ListItemData.liked)
                                                }
                                                ActionItem {
                                                    title: qsTr("Add to playlist")
                                                    imageSource: "asset:///images/ic_playlist_add.png"
                                                    enabled: ListItemData.kind == "track"
                                                    onTriggered: resultItem.ListItem.view.addToPlaylist(ListItemData.uri)
                                                }
                                            }
                                        ]
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
