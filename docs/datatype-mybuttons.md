---
tags:
  - datatype
---
# `MyButtons`

<!--dt-desc-start-->
Returns information about the name and command assigned to buttons
<!--dt-desc-end-->

## Members
<!--dt-members-start-->
### {{ renderMember(type='string', name='label', params='<#>') }}

:   The name assigned to &lt;Button#&gt;

### {{ renderMember(type='string', name='cmd', params='<#>') }}

:   The command assigned to &lt;Button#&gt;

<!--dt-members-end-->

## Examples
<!--dt-examples-start-->
- If using the .ini from [MQ2MyButtons](index.md#settings),
`/echo ${MyButtons.label[1]}` returns `Help`
<!--dt-examples-end-->

<!--dt-linkrefs-start-->
[string]: ../macroquest/reference/data-types/datatype-string.md
<!--dt-linkrefs-end-->
