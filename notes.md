LOW PRIORITY: PSD fill layers, add a warning for unsupported layer types to let the user know why their file looks different than in their source program
LOW PRIORITY: depth is a bit awkward with filters, the global depth cant really be respected
LOW PRIORITY: add a Pixelate filter that does bimodal mean, bimodal on luma
LOW PRIORITY: add a bloom/glow filter

Make PSD import use layer IDs for identification in anie rather than names where it makes sense, previously used only names, partially or fully done already I believe. Important for live link.

LOW PRIORITY: Currently meshing thresholds at a pretty arbitrary alpha value, figure out a better scheme for deciding a better alpha value depending on the actual image.

LOW PRIORITY: I have a dataset of PSDs in /home/gambot/Downloads/kemono_psd_1 that are to be used to verify AEs PSD compatibility. Extract the composite layer from the PSDs into PNGs to serve as GT, and in the background render out the same PSDs using AE and verify the correctness. This machine has a GPU and a display.


PROMPT: Review all the uncommitted changes made. Do not just focus on bugs, but architecture, design, and mental model.

PROMPT: I plan to contribute the uncommitted changes, can you prepare a commit, or split them into multiple such that its reviewable and prepared well for the maintainers? Be careful not to accidentally delete anything, you can create a backup if youre unsure.

PROMPT: Critically review the uncommitted changes in preparation of a commit. Dont just look for bugs, also review architecture, mental model, and code quality. Evaluate whether changes are truly the right changes, or just the easy changes. We have a high quality bar, we prioritize the right choices over minimal scope, so be very critical. Address the less ambiguous findings (that dont require my decision).
Unless there are any blockers, prepare a commit, think of what the split should be (if any), message, etc.
Take your time and be thorough, the changes are extensive, make no assumptions, this is not a time sensitive task.
You can use subagents for focused reviews.

PROMPT: Critically review the committed changes in preparation for a PR. Dont just look for bugs, also review architecture, mental model, and code quality. Evaluate whether changes are truly the right changes, or just the easy changes. We have a high quality bar, so be very critical. Address the less ambiguous findings if any (that dont require my decision).
You can use subagents for focused reviews.

PROMPT: Critically review the uncommitted changes in preparation of a commit. Dont just look for bugs, also review architecture, mental model, and code quality. Evaluate whether changes are truly the right changes, or just the easy changes. We have a high quality bar, we prioritize the right choices over minimal scope, so be very critical. Address the less ambiguous findings (that dont require my decision).
Unless there are any blockers, prepare a commit, think of what the split should be (if any), message, etc.
Take your time and be thorough, the changes are extensive, make no assumptions, this is not a time sensitive task.
You can use subagents for focused reviews.

filters are currently very inflexible, they dont have blend modes and have no mechanism for masking, this is artistically limiting

LOW PRIORITY: folders currently dont support normal blend mode, they only support pass-through, and pass-through has broken opacity, it changes per layer opacity instead of the whole folder composite

LOW PRIORITY: "AE renders folder opacity wrong, it uses it to modulate its childrens opacity rather than capturing the FBO before the folder is rendered, rendering it and then blending the temporary FBO with the current one based on folder opacity like other PSD tools do"
Can you verify this claim? Both parts of it. The reference PSD tools would in this case be python psdtools, krita, and gimp.


UNDECIDED: The tool dock should be a floating panel in the top left over the viewport, currently its full height but in 1080p at most only half of it is used, it wastes space.


UNDECIDED: The audio UI is a mess, add new audio track should be a button at the bottom with a plus icon. Removing audio tracks should be on each track as a trash icon.

Create a 5s looping tutorial video that shows in a modal on startup.

UNDECIDED: When no keys are selected, the user should be able to set default easing that is applied to new keys. This might be confusing to the user, there should be some indicator that makes it clear if the user is setting the easing on selected keys, or the default easing.
Examples where this might be confusing is if the user accidentally deselects a key -> changes easing -> is confused why easing didnt change -> users flow is broken. If the inputs got disabled when no key is selected, that would fix the flow, but then wouldnt allow to set default easing. Research what the best way to handle this is.

Onion skinning?



UNDECIDED: A segmented control or dropdown in the timeline to switch between showing the selected layer keys collapsed as a single lane; multiple lanes with multi value properties like position collapsed; multiple lanes with multi value properties expanded. When multiple layers are selected with different options, show none as active.




Publish to flathub.




How does the Auto easing mode apply to FFD and other things that arent single value? I noticed that the quick easing buttons are disabled for selections of most key types like Bone and Pose, Im wondering why, is that because those easing types arent supported for them? I made a big box selection with keyframes of couple of different types in ~/Downloads/AE ForTesting/AnimeEffects/fidohalfanim.anie and clicked the Auto quick easing button, but only keyframes of some types changed.
Graph editor currently doesnt show pose keyframes, this is a big limitation that I believe shouldnt exist. We kind of have a design limitation where bones dont really have a tree, its a single pose keyframe, the design is quite strange and foreign. It doesnt really allow per bone easing or keyframes, but it might have some advantages as well? The pose keyframe is essentially a vector with all the bones. Similarly to how Position is a vector of X and Y. How would this better be handled design wise? I suppose it being a single keyframe might be an ease of use win, but it probably trades flexibility heavily.

UNDECIDED: Change the bone visual, the joints should be a hollow ring and the bone a line going to the next circle down the hierarchy but stopping short by ~4px. Do not choose anything arbitrarily, make sure its designed. 

The easing can use the much cheaper cubic hermite calculation (demonstrated in ~/dev/curves/easing-demo.html) when the tangents are on thirds (always with auto easing). Verify this, scrutinize.

Explore the product design of visual selection as well as posing from ~/dev/Anie2D. Explore UX and HCI. Context is important, its not in vacuum.

Decompile Spine and Live2D, both are java so it should be fairly straightforward. Likely obfuscated, try to find out if there is an older build that is less obfuscated. Where are we lacking product design wise to be as successful as those two.

Does the exporters destination picker have competent fuzzy search? Is it hand-rolled or do we use a library? Should we use a library? Filters should presumably use fuzzy too unless there is a strong counter argument.

Add wave deform filter same or better than the one in wallpaper engine, research that. Probably should be post process as blur, else it will cause issues with mesh boundaries.
Add wallpaper engine style pulse filter.
Add a custom GLSL filter. Consider the interface, usually these are done poorly with bad UX, do it better.

The bottom part of the Properties panel has an info box, currently this is text only, but it would be nice to do it better so it can have e.g. things like badges for e.g. Undo/Redo instead of the awkward "Undone : X"
Make sure that multiline text select and copy works over multiple items without breaking anything. If required, you can keep it text only. Prioritize UX over minimal implementation.

In skeletal animation, the way perspective is typically done is with a custom mesh and per vertex weight or at least looser weight painting and a single bone that controls the viewing angle. The issue is that the transform from the bone is linear, just scaled based on the weight, this cannot achieve the same effect as a 3D transform, at steeper angles this drift off. Explore better approaches.

Develop a unity runtime.

Keep track of exported files in an export tab.

Either redesign the filters stack to a broader modifier stack or design a separate modifier stack and modifiers.

Thoughtful.

Add proper weight painting. Heatmap overlay, easy brush size and hardness.

The limitation of bones is that theyre 2D, so for animation it would be nice to get a pseudo-3D model and make that as intuitive as possible. Support painting depth/perspective and bones to use it.

Filters dont seem to apply until keyed. This might be a small bug, or a symptom of bad architecture and might be a deeper refactor.

Blend dropdown was rewritten to QML but has no scrollbar or separators, the latter is a regression.

QMenu has invisible separators. This probably happened when the background token changed. Fix.

The Filters add in properties is missing conventional keyboard controls.

The Filters cards seem to support only one expanded at a time. Should be any number.

Expanding/collapsing a Filter card in the properties panel seems to affect the layout of the header despite the header staying the same height. The headers layout should not shift.

Ctrl + Z shortcut for some reason ceased to work.

The brush alt + rmb drag UX is rough, shows no photoshop style brush preview and even though the user is dragging e.g. vertically, it changes both hardness and size, making it annoying to change one or the other, there is no photoshop style axis snap lock.

FFD and Filter mask paint menus in the Tools panel have broken layout, controls draw over each other.

Tooltips in the filter cards items draw behind the cards so theyre invisible.
