local helpers = require('test.functional.helpers')
local Screen = require('test.functional.ui.screen')
local clear, feed, nvim = helpers.clear, helpers.feed, helpers.nvim
local execute = helpers.execute

describe('Default highlight groups', function()
  -- Test the default attributes for highlight groups shown by the :highlight
  -- command
  local screen, hlgroup_colors

  setup(function()
    hlgroup_colors = {
      NonText = nvim('name_to_color', 'Blue'),
      Question = nvim('name_to_color', 'SeaGreen')
    }
  end)

  before_each(function()
    clear()
    screen = Screen.new()
    screen:attach()
  end)

  after_each(function()
    screen:detach()
  end)

  it('window status bar', function()
    screen:set_default_attr_ids({
      [1] = {reverse = true, bold = true},  -- StatusLine
      [2] = {reverse = true}                -- StatusLineNC
    })
    execute('sp', 'vsp', 'vsp')
    screen:expect([[
      ^                   {2:|}                {2:|}               |
      ~                   {2:|}~               {2:|}~              |
      ~                   {2:|}~               {2:|}~              |
      ~                   {2:|}~               {2:|}~              |
      ~                   {2:|}~               {2:|}~              |
      ~                   {2:|}~               {2:|}~              |
      {1:[No Name]            }{2:[No Name]        [No Name]      }|
                                                           |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      {2:[No Name]                                            }|
                                                           |
    ]])
    -- navigate to verify that the attributes are properly moved
    feed('<c-w>j')
    screen:expect([[
                          {2:|}                {2:|}               |
      ~                   {2:|}~               {2:|}~              |
      ~                   {2:|}~               {2:|}~              |
      ~                   {2:|}~               {2:|}~              |
      ~                   {2:|}~               {2:|}~              |
      ~                   {2:|}~               {2:|}~              |
      {2:[No Name]            [No Name]        [No Name]      }|
      ^                                                    |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      {1:[No Name]                                            }|
                                                           |
    ]])
    -- note that when moving to a window with small width nvim will increase
    -- the width of the new active window at the expense of a inactive window
    -- (upstream vim has the same behavior)
    feed('<c-w>k<c-w>l')
    screen:expect([[
                          {2:|}^                   {2:|}           |
      ~                   {2:|}~                   {2:|}~          |
      ~                   {2:|}~                   {2:|}~          |
      ~                   {2:|}~                   {2:|}~          |
      ~                   {2:|}~                   {2:|}~          |
      ~                   {2:|}~                   {2:|}~          |
      {2:[No Name]            }{1:[No Name]            }{2:[No Name]  }|
                                                           |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      {2:[No Name]                                            }|
                                                           |
    ]])
    feed('<c-w>l')
    screen:expect([[
                          {2:|}           {2:|}^                   |
      ~                   {2:|}~          {2:|}~                   |
      ~                   {2:|}~          {2:|}~                   |
      ~                   {2:|}~          {2:|}~                   |
      ~                   {2:|}~          {2:|}~                   |
      ~                   {2:|}~          {2:|}~                   |
      {2:[No Name]            [No Name]   }{1:[No Name]           }|
                                                           |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      {2:[No Name]                                            }|
                                                           |
    ]])
    feed('<c-w>h<c-w>h')
    screen:expect([[
      ^                   {2:|}                    {2:|}           |
      ~                   {2:|}~                   {2:|}~          |
      ~                   {2:|}~                   {2:|}~          |
      ~                   {2:|}~                   {2:|}~          |
      ~                   {2:|}~                   {2:|}~          |
      ~                   {2:|}~                   {2:|}~          |
      {1:[No Name]            }{2:[No Name]            [No Name]  }|
                                                           |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      {2:[No Name]                                            }|
                                                           |
    ]])
  end)

  it('insert mode text', function()
    feed('i')
    screen:expect([[
      ^                                                    |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      {1:-- INSERT --}                                         |
    ]], {[1] = {bold = true}})
  end)

  it('end of file markers', function()
    nvim('command', 'hi Normal guibg=black')
    screen:expect([[
      ^                                                    |
      {1:~                                                    }|
      {1:~                                                    }|
      {1:~                                                    }|
      {1:~                                                    }|
      {1:~                                                    }|
      {1:~                                                    }|
      {1:~                                                    }|
      {1:~                                                    }|
      {1:~                                                    }|
      {1:~                                                    }|
      {1:~                                                    }|
      {1:~                                                    }|
                                                           |
    ]], {[1] = {bold = true, foreground = hlgroup_colors.NonText}})
  end)

  it('"wait return" text', function()
    feed(':ls<cr>')
    screen:expect([[
      ~                                                    |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      ~                                                    |
      :ls                                                  |
        1 %a   "[No Name]"                    line 1       |
      {1:Press ENTER or type command to continue}^             |
    ]], {[1] = {bold = true, foreground = hlgroup_colors.Question}})
    feed('<cr>') --  skip the "Press ENTER..." state or tests will hang
  end)
end)
